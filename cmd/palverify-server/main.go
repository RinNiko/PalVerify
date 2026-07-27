package main

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"palverify/internal/serveragent"
)

func main() {
	executable, err := os.Executable()
	if err != nil {
		log.Fatal(err)
	}
	configPath := filepath.Join(
		filepath.Dir(executable),
		"server-config.json",
	)
	body, err := os.ReadFile(configPath)
	if err != nil {
		log.Fatalf("read config: %v", err)
	}
	var config serveragent.Config
	if err := json.Unmarshal(body, &config); err != nil {
		log.Fatalf("decode config: %v", err)
	}
	if config.DisciplineStatePath == "" {
		config.DisciplineStatePath = filepath.Join(
			filepath.Dir(executable),
			"discipline-state.json",
		)
	}
	if config.PalDefenderLogDirectory == "" {
		config.PalDefenderLogDirectory =
			defaultPalDefenderLogDirectory(executable)
	}
	if webhookURL := os.Getenv(
		"PALVERIFY_DISCORD_WEBHOOK_URL",
	); webhookURL != "" {
		config.DiscordWebhookURL = webhookURL
	}
	if err := config.Validate(); err != nil {
		log.Fatalf("invalid config: %v", err)
	}

	client := &http.Client{Timeout: 12 * time.Second}
	startupDeadline := time.Now().Add(30 * time.Second)
	startupReady := false
	run := func() {
		ctx, cancel := context.WithTimeout(
			context.Background(),
			10*time.Second,
		)
		defer cancel()
		enforced, syncErr := serveragent.SyncOnce(
			ctx,
			client,
			config,
			time.Now(),
			log.Default(),
		)
		if syncErr != nil {
			if !startupReady &&
				time.Now().Before(startupDeadline) &&
				serveragent.IsTransientStartupError(syncErr) {
				return
			}
			log.Printf("[PalVerify] sync failed: %v", syncErr)
			return
		}
		startupReady = true
		if enforced != 0 {
			log.Printf("[PalVerify] enforcement_actions=%d", enforced)
		}
	}

	log.Printf(
		"[PalVerify] server agent started version=1.0 interval=%ds admin_audit=true discord_audit=%t",
		config.IntervalSeconds,
		config.DiscordWebhookURL != "",
	)
	run()
	ticker := time.NewTicker(time.Duration(config.IntervalSeconds) * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		run()
	}
}

func defaultPalDefenderLogDirectory(executable string) string {
	return filepath.Clean(filepath.Join(
		filepath.Dir(executable),
		"..",
		"..",
		"..",
		"..",
		"PalDefender",
		"Logs",
	))
}
