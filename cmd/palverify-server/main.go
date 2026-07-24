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
		kicks, syncErr := serveragent.SyncOnce(
			ctx,
			client,
			config,
			time.Now(),
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
		if kicks != 0 {
			log.Printf("[PalVerify] kicked=%d", kicks)
		}
	}

	log.Printf(
		"[PalVerify] server agent started version=1.0 interval=%ds",
		config.IntervalSeconds,
	)
	run()
	ticker := time.NewTicker(time.Duration(config.IntervalSeconds) * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		run()
	}
}
