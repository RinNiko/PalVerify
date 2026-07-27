package main

import (
	"context"
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"os"
	"strings"
	"time"

	"palverify/internal/coordinator"
)

func main() {
	serverToken := strings.TrimSpace(os.Getenv("PALVERIFY_SERVER_TOKEN"))
	if serverToken == "" {
		log.Fatal("PALVERIFY_SERVER_TOKEN is required")
	}

	allowed := make(map[string]coordinator.AllowedMod)
	if err := json.Unmarshal(
		[]byte(os.Getenv("PALVERIFY_ALLOWED_MODS_JSON")),
		&allowed,
	); err != nil || len(allowed) == 0 {
		log.Fatal("PALVERIFY_ALLOWED_MODS_JSON must contain the whitelist")
	}
	var launcherManifest coordinator.LauncherManifest
	if err := json.Unmarshal(
		[]byte(os.Getenv("PALVERIFY_LAUNCHER_MANIFEST_JSON")),
		&launcherManifest,
	); err != nil ||
		launcherManifest.LauncherVersion == "" ||
		launcherManifest.MinimumLauncherVersion == "" ||
		launcherManifest.LauncherDownloadURL == "" ||
		launcherManifest.LauncherSHA256 == "" ||
		launcherManifest.PalVerifyVersion == "" ||
		launcherManifest.RequiredPalworldBuildID == "" ||
		launcherManifest.PalworldVersion == "" {
		log.Fatal(
			"PALVERIFY_LAUNCHER_MANIFEST_JSON must contain launcher and game versions",
		)
	}
	palVerifyAllowed, exists := allowed["PalVerify"]
	if !exists ||
		palVerifyAllowed.Version == "" ||
		palVerifyAllowed.Digest == "" {
		log.Fatal("PalVerify must exist in PALVERIFY_ALLOWED_MODS_JSON")
	}
	if launcherManifest.PalVerifyPackageDigest == "" {
		launcherManifest.PalVerifyPackageDigest = palVerifyAllowed.Digest
	}

	store := coordinator.NewStore(coordinator.Config{
		GracePeriod:      20 * time.Second,
		ReportMaxAge:     15 * time.Second,
		ChallengeMaxAge:  15 * time.Second,
		AllowedMods:      allowed,
		LauncherManifest: launcherManifest,
	})
	cachePath := strings.TrimSpace(
		os.Getenv("PALVERIFY_MANIFEST_CACHE_PATH"),
	)
	if cachePath == "" {
		cachePath = "/var/lib/palverify-coordinator/release-manifest.json"
	}
	if cached, err := coordinator.LoadReleaseManifest(cachePath); err == nil {
		if changed, applyErr := store.ApplyReleaseManifest(cached); applyErr != nil {
			log.Printf("cached release manifest rejected: %v", applyErr)
		} else if changed {
			log.Printf(
				"cached release manifest restored launcher=%s palverify=%s",
				cached.LauncherVersion,
				cached.PalVerifyVersion,
			)
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		log.Printf("cached release manifest unavailable: %v", err)
	}
	go coordinator.RunManifestSync(
		context.Background(),
		store,
		coordinator.NewGitHubManifestFetcher(nil),
		time.Minute,
		cachePath,
		log.Default(),
	)
	var audit coordinator.AuditSink
	webhookURL := strings.TrimSpace(
		os.Getenv("PALVERIFY_DISCORD_WEBHOOK_URL"),
	)
	if webhookURL != "" {
		sink, err := coordinator.NewDiscordAuditSink(
			webhookURL,
			nil,
			log.Default(),
		)
		if err != nil {
			log.Fatalf("invalid Discord audit configuration: %v", err)
		}
		audit = sink
	}
	handler := coordinator.NewHandlerWithAudit(
		store,
		serverToken,
		time.Now,
		log.Default(),
		audit,
	)
	server := &http.Server{
		Addr:              "127.0.0.1:18801",
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      10 * time.Second,
		IdleTimeout:       30 * time.Second,
		MaxHeaderBytes:    16 * 1024,
	}
	log.Printf(
		"PalVerify coordinator listening on %s discord_audit=%t",
		server.Addr,
		audit != nil,
	)
	if err := server.ListenAndServe(); !errors.Is(err, http.ErrServerClosed) {
		log.Fatal(err)
	}
}
