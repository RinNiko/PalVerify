package main

import (
	"path/filepath"
	"testing"
)

func TestDefaultPalDefenderLogDirectoryFromServerExecutable(t *testing.T) {
	t.Parallel()

	executable := filepath.Join(
		`C:\PalServer`,
		"Pal",
		"Binaries",
		"Win64",
		"ue4ss",
		"Mods",
		"PalVerify",
		"Scripts",
		"PalVerifyServer.exe",
	)
	expected := filepath.Join(
		`C:\PalServer`,
		"Pal",
		"Binaries",
		"Win64",
		"PalDefender",
		"Logs",
	)

	if actual := defaultPalDefenderLogDirectory(executable); actual != expected {
		t.Fatalf("unexpected PalDefender log directory: got %q want %q", actual, expected)
	}
}
