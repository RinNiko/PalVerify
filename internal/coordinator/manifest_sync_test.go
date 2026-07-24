package coordinator

import (
	"context"
	"errors"
	"path/filepath"
	"testing"
)

const (
	testDigestA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	testDigestB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
)

func testLauncherManifest(
	launcherVersion string,
	palVerifyVersion string,
	packageDigest string,
) LauncherManifest {
	return LauncherManifest{
		LauncherVersion:         launcherVersion,
		MinimumLauncherVersion:  launcherVersion,
		LauncherDownloadURL:     "https://github.com/RinNiko/PalVerify/releases/download/v" + launcherVersion + "/Pal3Mien-Setup.exe",
		LauncherSHA256:          testDigestA,
		PalVerifyVersion:        palVerifyVersion,
		PalVerifyPackageDigest:  packageDigest,
		RequiredPalworldBuildID: "24181527",
		PalworldVersion:         "v1.0.1.100619",
		ServerOnline:            true,
		ServerAddress:           "lantica.dathost.net:28709",
		WebsiteURL:              "https://palworld-3-mien-website.vercel.app/",
		NewsURL:                 "https://palworld-3-mien-website.vercel.app/",
	}
}

func TestApplyReleaseManifestUpdatesLauncherAndWhitelist(t *testing.T) {
	store := NewStore(Config{
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.7",
				Digest:  testDigestA,
			},
		},
		LauncherManifest: testLauncherManifest("0.5.12", "0.5.7", testDigestA),
	})

	changed, err := store.ApplyReleaseManifest(
		testLauncherManifest("0.5.13", "0.5.8", testDigestB),
	)
	if err != nil {
		t.Fatalf("apply manifest: %v", err)
	}
	if !changed {
		t.Fatal("expected manifest to change")
	}

	manifest := store.LauncherManifest()
	if manifest.LauncherVersion != "0.5.13" {
		t.Fatalf("unexpected launcher version: %q", manifest.LauncherVersion)
	}
	allowed := store.AllowedMod("PalVerify")
	if allowed.Version != "0.5.8" || allowed.Digest != testDigestB {
		t.Fatalf("unexpected PalVerify whitelist: %#v", allowed)
	}
}

func TestApplyReleaseManifestRejectsInvalidDigestAndKeepsWhitelist(
	t *testing.T,
) {
	store := NewStore(Config{
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.7",
				Digest:  testDigestA,
			},
		},
		LauncherManifest: testLauncherManifest("0.5.12", "0.5.7", testDigestA),
	})
	manifest := testLauncherManifest("0.5.13", "0.5.8", "not-a-sha256")

	if _, err := store.ApplyReleaseManifest(manifest); err == nil {
		t.Fatal("expected invalid package digest to be rejected")
	}
	allowed := store.AllowedMod("PalVerify")
	if allowed.Version != "0.5.7" || allowed.Digest != testDigestA {
		t.Fatalf("whitelist changed after invalid manifest: %#v", allowed)
	}
}

func TestApplyReleaseManifestRejectsDowngrade(t *testing.T) {
	store := NewStore(Config{
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.8",
				Digest:  testDigestB,
			},
		},
		LauncherManifest: testLauncherManifest("0.5.13", "0.5.8", testDigestB),
	})

	if _, err := store.ApplyReleaseManifest(
		testLauncherManifest("0.5.12", "0.5.7", testDigestA),
	); err == nil {
		t.Fatal("expected downgrade to be rejected")
	}
}

func TestApplyReleaseManifestAllowsNewLauncherWithRebuiltSameVersionPayload(
	t *testing.T,
) {
	store := NewStore(Config{
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.7",
				Digest:  testDigestA,
			},
		},
		LauncherManifest: testLauncherManifest("0.5.12", "0.5.7", testDigestA),
	})

	changed, err := store.ApplyReleaseManifest(
		testLauncherManifest("0.5.13", "0.5.7", testDigestB),
	)
	if err != nil {
		t.Fatalf("apply rebuilt payload: %v", err)
	}
	if !changed {
		t.Fatal("expected rebuilt payload to update the whitelist")
	}
	allowed := store.AllowedMod("PalVerify")
	if allowed.Version != "0.5.7" || allowed.Digest != testDigestB {
		t.Fatalf("unexpected rebuilt payload whitelist: %#v", allowed)
	}
}

func TestApplyReleaseManifestRejectsDigestMutationAtSameLauncherVersion(
	t *testing.T,
) {
	store := NewStore(Config{
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.7",
				Digest:  testDigestA,
			},
		},
		LauncherManifest: testLauncherManifest("0.5.12", "0.5.7", testDigestA),
	})

	if _, err := store.ApplyReleaseManifest(
		testLauncherManifest("0.5.12", "0.5.7", testDigestB),
	); err == nil {
		t.Fatal("expected same-version digest mutation to be rejected")
	}
}

func TestCompareVersionsAcceptsOfficialTwoPartVersions(t *testing.T) {
	order, err := compareVersions("1.0", "0.5.13")
	if err != nil {
		t.Fatalf("official two-part version must be valid: %v", err)
	}
	if order != 1 {
		t.Fatalf("expected 1.0 to be newer than 0.5.13, got %d", order)
	}

	order, err = compareVersions("1.0", "1.0.0")
	if err != nil {
		t.Fatalf("two and three-part versions must compare: %v", err)
	}
	if order != 0 {
		t.Fatalf("expected 1.0 and 1.0.0 to be equal, got %d", order)
	}
}

func TestApplyReleaseManifestRejectsUntrustedDownloadURL(t *testing.T) {
	store := NewStore(Config{})
	manifest := testLauncherManifest("0.5.13", "0.5.8", testDigestB)
	manifest.LauncherDownloadURL = "https://example.com/Pal3Mien-Setup.exe"

	if _, err := store.ApplyReleaseManifest(manifest); err == nil {
		t.Fatal("expected untrusted launcher URL to be rejected")
	}
}

func TestSyncManifestOnceKeepsLastKnownGoodOnFetchFailure(t *testing.T) {
	store := NewStore(Config{
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.7",
				Digest:  testDigestA,
			},
		},
		LauncherManifest: testLauncherManifest("0.5.12", "0.5.7", testDigestA),
	})
	fetcher := func(context.Context) (LauncherManifest, error) {
		return LauncherManifest{}, errors.New("temporary GitHub failure")
	}

	if _, err := SyncManifestOnce(context.Background(), store, fetcher); err == nil {
		t.Fatal("expected fetch error")
	}
	allowed := store.AllowedMod("PalVerify")
	if allowed.Digest != testDigestA {
		t.Fatalf("last known good digest was not preserved: %#v", allowed)
	}
}

func TestSyncManifestOnceAppliesFetchedManifest(t *testing.T) {
	store := NewStore(Config{
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.7",
				Digest:  testDigestA,
			},
		},
		LauncherManifest: testLauncherManifest("0.5.12", "0.5.7", testDigestA),
	})
	fetcher := func(context.Context) (LauncherManifest, error) {
		return testLauncherManifest("0.5.13", "0.5.8", testDigestB), nil
	}

	changed, err := SyncManifestOnce(context.Background(), store, fetcher)
	if err != nil {
		t.Fatalf("sync manifest: %v", err)
	}
	if !changed {
		t.Fatal("expected fetched manifest to be applied")
	}
	if store.AllowedMod("PalVerify").Digest != testDigestB {
		t.Fatal("fetched digest was not applied")
	}
}

func TestReleaseManifestCacheRoundTrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "release-manifest.json")
	expected := testLauncherManifest("0.5.13", "0.5.8", testDigestB)

	if err := SaveReleaseManifest(path, expected); err != nil {
		t.Fatalf("save cache: %v", err)
	}
	actual, err := LoadReleaseManifest(path)
	if err != nil {
		t.Fatalf("load cache: %v", err)
	}
	if actual != expected {
		t.Fatalf("cache mismatch: got %#v want %#v", actual, expected)
	}
}
