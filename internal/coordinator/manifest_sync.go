package coordinator

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

const (
	WebsiteManifestURL = "https://ae3mien.net/api/palverify/v1/launcher/manifest"
	maxManifestBytes   = 32 * 1024
)

type ManifestFetcher func(context.Context) (LauncherManifest, error)

func SyncManifestOnce(
	ctx context.Context,
	store *Store,
	fetcher ManifestFetcher,
) (bool, error) {
	if store == nil {
		return false, errors.New("manifest sync store is required")
	}
	if fetcher == nil {
		return false, errors.New("manifest fetcher is required")
	}
	manifest, err := fetcher(ctx)
	if err != nil {
		return false, fmt.Errorf("fetch release manifest: %w", err)
	}
	return store.ApplyReleaseManifest(manifest)
}

func RunManifestSync(
	ctx context.Context,
	store *Store,
	fetcher ManifestFetcher,
	interval time.Duration,
	cachePath string,
	logger *log.Logger,
) {
	if interval < time.Minute {
		interval = time.Minute
	}
	if logger == nil {
		logger = log.Default()
	}
	first := true
	sync := func() {
		requestContext, cancel := context.WithTimeout(ctx, 20*time.Second)
		defer cancel()
		changed, err := SyncManifestOnce(requestContext, store, fetcher)
		if err != nil {
			logger.Printf("release manifest sync failed: %v", err)
			return
		}
		manifest := store.LauncherManifest()
		if cachePath != "" && (changed || first) {
			if err := SaveReleaseManifest(cachePath, manifest); err != nil {
				logger.Printf("release manifest cache failed: %v", err)
			}
		}
		if changed || first {
			logger.Printf(
				"release manifest checked changed=%t launcher=%s palverify=%s",
				changed,
				manifest.LauncherVersion,
				manifest.PalVerifyVersion,
			)
		}
		first = false
	}

	sync()
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			sync()
		}
	}
}

func SaveReleaseManifest(path string, manifest LauncherManifest) error {
	if err := ValidateReleaseManifest(manifest); err != nil {
		return err
	}
	directory := filepath.Dir(path)
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return fmt.Errorf("create manifest cache directory: %w", err)
	}
	content, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return fmt.Errorf("encode manifest cache: %w", err)
	}
	content = append(content, '\n')

	file, err := os.CreateTemp(directory, ".release-manifest-*")
	if err != nil {
		return fmt.Errorf("create manifest cache: %w", err)
	}
	temporaryPath := file.Name()
	defer os.Remove(temporaryPath)
	if err := file.Chmod(0o600); err != nil {
		file.Close()
		return fmt.Errorf("protect manifest cache: %w", err)
	}
	if _, err := file.Write(content); err != nil {
		file.Close()
		return fmt.Errorf("write manifest cache: %w", err)
	}
	if err := file.Sync(); err != nil {
		file.Close()
		return fmt.Errorf("sync manifest cache: %w", err)
	}
	if err := file.Close(); err != nil {
		return fmt.Errorf("close manifest cache: %w", err)
	}
	if err := os.Rename(temporaryPath, path); err != nil {
		return fmt.Errorf("replace manifest cache: %w", err)
	}
	return nil
}

func LoadReleaseManifest(path string) (LauncherManifest, error) {
	file, err := os.Open(path)
	if err != nil {
		return LauncherManifest{}, fmt.Errorf("open manifest cache: %w", err)
	}
	defer file.Close()
	content, err := io.ReadAll(io.LimitReader(file, maxManifestBytes+1))
	if err != nil {
		return LauncherManifest{}, fmt.Errorf("read manifest cache: %w", err)
	}
	if len(content) > maxManifestBytes {
		return LauncherManifest{}, errors.New("manifest cache is too large")
	}
	var manifest LauncherManifest
	if err := json.Unmarshal(content, &manifest); err != nil {
		return LauncherManifest{}, fmt.Errorf("decode manifest cache: %w", err)
	}
	if err := ValidateReleaseManifest(manifest); err != nil {
		return LauncherManifest{}, err
	}
	return manifest, nil
}

func NewGitHubManifestFetcher(client *http.Client) ManifestFetcher {
	if client == nil {
		client = &http.Client{Timeout: 20 * time.Second}
	}
	return func(ctx context.Context) (LauncherManifest, error) {
		request, err := http.NewRequestWithContext(
			ctx,
			http.MethodGet,
			WebsiteManifestURL,
			nil,
		)
		if err != nil {
			return LauncherManifest{}, err
		}
		request.Header.Set("Accept", "application/json")
		request.Header.Set("User-Agent", "PalVerify-Coordinator/1.0")

		response, err := client.Do(request)
		if err != nil {
			return LauncherManifest{}, err
		}
		defer response.Body.Close()
		if response.StatusCode != http.StatusOK {
			return LauncherManifest{}, fmt.Errorf(
				"website manifest endpoint returned HTTP %d",
				response.StatusCode,
			)
		}

		body, err := io.ReadAll(io.LimitReader(
			response.Body,
			maxManifestBytes+1,
		))
		if err != nil {
			return LauncherManifest{}, err
		}
		if len(body) > maxManifestBytes {
			return LauncherManifest{}, errors.New("release manifest is too large")
		}
		var manifest LauncherManifest
		if err := json.Unmarshal(body, &manifest); err != nil {
			return LauncherManifest{}, fmt.Errorf(
				"decode release manifest: %w",
				err,
			)
		}
		if err := ValidateReleaseManifest(manifest); err != nil {
			return LauncherManifest{}, err
		}
		return manifest, nil
	}
}

func (store *Store) ApplyReleaseManifest(
	manifest LauncherManifest,
) (bool, error) {
	if err := ValidateReleaseManifest(manifest); err != nil {
		return false, err
	}
	manifest.LauncherSHA256 = strings.ToLower(manifest.LauncherSHA256)
	manifest.PalVerifyPackageDigest = strings.ToLower(
		manifest.PalVerifyPackageDigest,
	)

	store.mu.Lock()
	defer store.mu.Unlock()

	current := store.config.LauncherManifest
	if current.LauncherVersion != "" {
		order, err := compareVersions(
			manifest.LauncherVersion,
			current.LauncherVersion,
		)
		if err != nil {
			return false, err
		}
		if order < 0 {
			return false, errors.New("launcher manifest downgrade rejected")
		}
		if order == 0 &&
			!strings.EqualFold(
				manifest.PalVerifyPackageDigest,
				current.PalVerifyPackageDigest,
			) {
			return false, errors.New(
				"package digest changed without a launcher version bump",
			)
		}
	}
	if current.PalVerifyVersion != "" {
		order, err := compareVersions(
			manifest.PalVerifyVersion,
			current.PalVerifyVersion,
		)
		if err != nil {
			return false, err
		}
		if order < 0 {
			return false, errors.New("PalVerify manifest downgrade rejected")
		}
	}

	allowed := store.config.AllowedMods["PalVerify"]
	if current == manifest &&
		allowed.Version == manifest.PalVerifyVersion &&
		strings.EqualFold(
			allowed.Digest,
			manifest.PalVerifyPackageDigest,
		) {
		return false, nil
	}
	if store.config.AllowedMods == nil {
		store.config.AllowedMods = make(map[string]AllowedMod)
	}
	updated := AllowedMod{
		Version: manifest.PalVerifyVersion,
		Digest:  manifest.PalVerifyPackageDigest,
	}
	if allowed.Version != "" && allowed.Digest != "" &&
		(allowed.Version != updated.Version ||
			!strings.EqualFold(allowed.Digest, updated.Digest)) {
		updated.CompatiblePackages = []AllowedPackage{{
			Version: allowed.Version,
			Digest:  allowed.Digest,
		}}
	}
	store.config.AllowedMods["PalVerify"] = updated
	store.config.LauncherManifest = manifest
	return true, nil
}

func ValidateReleaseManifest(manifest LauncherManifest) error {
	required := map[string]string{
		"launcherVersion":         manifest.LauncherVersion,
		"minimumLauncherVersion":  manifest.MinimumLauncherVersion,
		"launcherDownloadUrl":     manifest.LauncherDownloadURL,
		"launcherSha256":          manifest.LauncherSHA256,
		"palVerifyVersion":        manifest.PalVerifyVersion,
		"palVerifyPackageDigest":  manifest.PalVerifyPackageDigest,
		"requiredPalworldBuildId": manifest.RequiredPalworldBuildID,
		"palworldVersion":         manifest.PalworldVersion,
	}
	for name, value := range required {
		if strings.TrimSpace(value) == "" {
			return fmt.Errorf("release manifest missing %s", name)
		}
	}
	if !isSHA256(manifest.LauncherSHA256) {
		return errors.New("release manifest has invalid launcher SHA-256")
	}
	if !isSHA256(manifest.PalVerifyPackageDigest) {
		return errors.New("release manifest has invalid package digest")
	}
	if _, err := compareVersions(
		manifest.LauncherVersion,
		manifest.MinimumLauncherVersion,
	); err != nil {
		return err
	}
	if _, err := compareVersions(manifest.PalVerifyVersion, "0.0.0"); err != nil {
		return err
	}
	downloadURL, err := url.Parse(manifest.LauncherDownloadURL)
	if err != nil ||
		downloadURL.Scheme != "https" ||
		!strings.EqualFold(downloadURL.Hostname(), "github.com") ||
		!strings.HasPrefix(
			downloadURL.EscapedPath(),
			"/RinNiko/PalVerify/releases/download/",
		) {
		return errors.New("release manifest has untrusted launcher URL")
	}
	return nil
}

func isSHA256(value string) bool {
	if len(value) != 64 {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil
}

func compareVersions(left string, right string) (int, error) {
	parse := func(value string) ([3]int, error) {
		var result [3]int
		value = strings.TrimPrefix(strings.TrimSpace(value), "v")
		parts := strings.Split(value, ".")
		if len(parts) < 2 || len(parts) > len(result) {
			return result, fmt.Errorf("invalid version %q", value)
		}
		for index, part := range parts {
			number, err := strconv.Atoi(part)
			if err != nil || number < 0 {
				return result, fmt.Errorf("invalid version %q", value)
			}
			result[index] = number
		}
		return result, nil
	}
	leftParts, err := parse(left)
	if err != nil {
		return 0, err
	}
	rightParts, err := parse(right)
	if err != nil {
		return 0, err
	}
	for index := range leftParts {
		if leftParts[index] < rightParts[index] {
			return -1, nil
		}
		if leftParts[index] > rightParts[index] {
			return 1, nil
		}
	}
	return 0, nil
}
