package serveragent

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"path/filepath"
	"strings"
	"time"
)

type Config struct {
	Endpoint                string `json:"endpoint"`
	ServerToken             string `json:"serverToken"`
	ServerID                string `json:"serverId"`
	RestURL                 string `json:"restUrl"`
	RestToken               string `json:"restToken"`
	IntervalSeconds         int    `json:"intervalSeconds"`
	DiscordWebhookURL       string `json:"discordWebhookUrl,omitempty"`
	DisciplineStatePath     string `json:"disciplineStatePath,omitempty"`
	PalDefenderLogDirectory string `json:"palDefenderLogDirectory,omitempty"`
}

func (config Config) Validate() error {
	endpoint, err := url.Parse(config.Endpoint)
	if err != nil || endpoint.Hostname() == "" {
		return errors.New("coordinator endpoint is invalid")
	}
	if endpoint.Scheme != "https" &&
		!(endpoint.Scheme == "http" && isLoopback(endpoint.Hostname())) {
		return errors.New("coordinator endpoint must use HTTPS")
	}
	restURL, err := url.Parse(config.RestURL)
	if err != nil || restURL.Hostname() == "" ||
		!isLoopback(restURL.Hostname()) ||
		(restURL.Scheme != "http" && restURL.Scheme != "https") {
		return errors.New("PalDefender REST URL must be loopback HTTP")
	}
	if strings.TrimSpace(config.ServerToken) == "" ||
		strings.TrimSpace(config.ServerID) == "" ||
		strings.TrimSpace(config.RestToken) == "" {
		return errors.New("config is missing a required credential")
	}
	if config.IntervalSeconds < 1 || config.IntervalSeconds > 60 {
		return errors.New("intervalSeconds must be between 1 and 60")
	}
	if err := validateDiscordWebhookURL(config.DiscordWebhookURL); err != nil {
		return err
	}
	if strings.TrimSpace(config.PalDefenderLogDirectory) != "" &&
		!filepath.IsAbs(config.PalDefenderLogDirectory) {
		return errors.New("PalDefender log directory must be absolute")
	}
	return nil
}

func isLoopback(host string) bool {
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

func IsTransientStartupError(err error) bool {
	if err == nil {
		return false
	}
	message := strings.ToLower(err.Error())
	return strings.Contains(
		message,
		"paldefender players returned http 403",
	) || strings.Contains(message, "connection refused") ||
		strings.Contains(message, "connectex")
}

type player struct {
	Name   string `json:"Name"`
	Status string `json:"Status"`
	UserID string `json:"UserId"`
	IP     string `json:"IP"`
}

type playersDocument struct {
	Players []player `json:"Players"`
}

type onlinePlayer struct {
	UserID string `json:"userId"`
	Name   string `json:"name"`
}

type evaluateRequest struct {
	ServerID string         `json:"serverId"`
	SentAt   string         `json:"sentAt"`
	Players  []onlinePlayer `json:"players"`
}

type decision struct {
	UserID     string   `json:"userId"`
	Name       string   `json:"name"`
	IP         string   `json:"-"`
	Action     string   `json:"action"`
	Reason     string   `json:"reason"`
	Mods       []string `json:"mods"`
	Violations []string `json:"violations"`
	// Detail is the coordinator's compact, operator-facing explanation of
	// the decision (e.g. "NO_VALID_REPORT", "STALE_REPORT age=16s",
	// "PalVerify:VERSION_MISMATCH"). It never carries file/process
	// inventories, only rule codes and bounded scalar facts.
	Detail string `json:"detail"`
}

type evaluateResponse struct {
	Decisions []decision `json:"decisions"`
}

func SyncOnce(
	ctx context.Context,
	client *http.Client,
	config Config,
	now time.Time,
	logger *log.Logger,
) (int, error) {
	state, err := loadDisciplineState(config.DisciplineStatePath)
	if err != nil {
		return 0, err
	}
	if err := processExpiredTemporaryBans(
		ctx,
		client,
		config,
		now,
		&state,
		logger,
	); err != nil {
		return 0, err
	}
	if collected, collectErr := collectPalDefenderAudits(
		config,
		&state,
		now,
	); collectErr != nil {
		if logger != nil {
			logger.Printf("[PalVerify] PalDefender log bridge failed: %v", collectErr)
		}
	} else if collected != 0 &&
		strings.TrimSpace(config.DisciplineStatePath) != "" {
		if err := saveDisciplineState(config.DisciplineStatePath, state); err != nil {
			return 0, err
		}
	} else if state.palDefenderChanged &&
		strings.TrimSpace(config.DisciplineStatePath) != "" {
		if err := saveDisciplineState(config.DisciplineStatePath, state); err != nil {
			return 0, err
		}
	}
	flushDiscordAudits(ctx, client, config, &state, logger)
	if processed, moderationErr := processAdminModerationAction(
		ctx,
		client,
		config,
	); moderationErr != nil {
		if logger != nil {
			logger.Printf("[PalVerify] admin moderation bridge failed: %v", moderationErr)
		}
	} else if processed && logger != nil {
		logger.Printf("[PalVerify] admin moderation action processed")
	}

	document, err := fetchPlayers(ctx, client, config)
	if err != nil {
		return 0, err
	}

	online := make([]onlinePlayer, 0, len(document.Players))
	playerIPs := make(map[string]string, len(document.Players))
	for _, candidate := range document.Players {
		if !strings.EqualFold(strings.TrimSpace(candidate.Status), "online") {
			continue
		}
		userID := strings.TrimSpace(candidate.UserID)
		name := strings.TrimSpace(candidate.Name)
		if userID == "" {
			continue
		}
		if name == "" {
			name = "unknown"
		}
		playerIPs[userID] = strings.TrimSpace(candidate.IP)
		online = append(online, onlinePlayer{UserID: userID, Name: name})
	}
	seenOnline := make(map[string]struct{}, len(online))
	for _, candidate := range online {
		seenOnline[candidate.UserID] = struct{}{}
	}
	for userID, connected := range state.ConnectedPlayers {
		if _, exists := seenOnline[userID]; exists {
			continue
		}
		name := strings.TrimSpace(connected.Name)
		if name == "" {
			name = "unknown"
		}
		playerIPs[userID] = strings.TrimSpace(connected.IP)
		online = append(online, onlinePlayer{UserID: userID, Name: name})
	}

	body, err := json.Marshal(evaluateRequest{
		ServerID: config.ServerID,
		SentAt:   now.UTC().Format(time.RFC3339Nano),
		Players:  online,
	})
	if err != nil {
		return 0, fmt.Errorf("encode evaluation: %w", err)
	}
	request, err := http.NewRequestWithContext(
		ctx,
		http.MethodPost,
		config.Endpoint,
		bytes.NewReader(body),
	)
	if err != nil {
		return 0, fmt.Errorf("create evaluation request: %w", err)
	}
	request.Header.Set("Authorization", "Bearer "+config.ServerToken)
	request.Header.Set("Content-Type", "application/json; charset=utf-8")

	response, err := client.Do(request)
	if err != nil {
		return 0, fmt.Errorf("evaluate players: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		_, _ = io.Copy(io.Discard, response.Body)
		return 0, fmt.Errorf(
			"coordinator returned HTTP %d",
			response.StatusCode,
		)
	}
	var evaluation evaluateResponse
	if err := json.NewDecoder(
		io.LimitReader(response.Body, 256*1024),
	).Decode(&evaluation); err != nil {
		return 0, fmt.Errorf("decode evaluation: %w", err)
	}

	kicks := 0
	for _, result := range evaluation.Decisions {
		result.IP = playerIPs[result.UserID]
		if result.Action != "kick" {
			continue
		}
		if result.Reason == "INTEGRITY_VIOLATION" {
			enforced, err := enforceIntegrityDiscipline(
				ctx,
				client,
				config,
				now,
				result,
				&state,
				logger,
			)
			if err != nil {
				return kicks, err
			}
			if enforced {
				kicks++
			}
			continue
		}
		reason := formatKickReason(result)
		if err := kickPlayer(
			ctx,
			client,
			config,
			result.UserID,
			reason,
		); err != nil {
			return kicks, err
		}
		kicks++
		if logger != nil {
			logger.Printf(
				"[PalVerify] kicked user=%s reason=%s mods=%s detail=%q",
				compactUserID(result.UserID),
				result.Reason,
				strings.Join(result.Mods, ","),
				result.Detail,
			)
		}
		queueKickAudit(config, &state, now, result)
		if strings.TrimSpace(config.DisciplineStatePath) != "" &&
			len(state.PendingAudits) != 0 {
			if err := saveDisciplineState(
				config.DisciplineStatePath,
				state,
			); err != nil {
				return kicks, err
			}
		}
	}
	flushDiscordAudits(ctx, client, config, &state, logger)
	return kicks, nil
}

func formatKickReason(result decision) string {
	reason := "PalVerify: " + result.Reason
	details := result.Mods
	if len(result.Violations) != 0 {
		details = result.Violations
	}
	switch {
	case len(details) != 0:
		reason += " (" + strings.Join(details, ",") + ")"
	case strings.TrimSpace(result.Detail) != "":
		// Fall back to the coordinator diagnostic detail (e.g.
		// "NO_VALID_REPORT", "STALE_REPORT age=16s") so a bare reason code like
		// MISSING_PALVERIFY still tells the player and the log what happened.
		reason += " (" + strings.TrimSpace(result.Detail) + ")"
	}
	if len(reason) > 180 {
		reason = reason[:180]
	}
	return reason
}

// compactUserID shortens a Steam user id for logs so the full identity is not
// written verbatim to the game server console, matching the coordinator's
// compact logging policy.
func compactUserID(userID string) string {
	userID = strings.TrimSpace(userID)
	if len(userID) <= 12 {
		return userID
	}
	return userID[:8] + "..." + userID[len(userID)-4:]
}

func fetchPlayers(
	ctx context.Context,
	client *http.Client,
	config Config,
) (playersDocument, error) {
	endpoint := strings.TrimRight(config.RestURL, "/") + "/v1/pdapi/players"
	request, err := http.NewRequestWithContext(
		ctx,
		http.MethodGet,
		endpoint,
		nil,
	)
	if err != nil {
		return playersDocument{}, fmt.Errorf("create players request: %w", err)
	}
	request.Header.Set("Authorization", "Bearer "+config.RestToken)
	request.Header.Set("Accept", "application/json")
	response, err := client.Do(request)
	if err != nil {
		return playersDocument{}, fmt.Errorf("read players: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		_, _ = io.Copy(io.Discard, response.Body)
		return playersDocument{}, fmt.Errorf(
			"PalDefender players returned HTTP %d",
			response.StatusCode,
		)
	}
	var document playersDocument
	if err := json.NewDecoder(
		io.LimitReader(response.Body, 2*1024*1024),
	).Decode(&document); err != nil {
		return playersDocument{}, fmt.Errorf("decode players: %w", err)
	}
	return document, nil
}

func kickPlayer(
	ctx context.Context,
	client *http.Client,
	config Config,
	userID string,
	reason string,
) error {
	body, err := json.Marshal(map[string]string{"Reason": reason})
	if err != nil {
		return fmt.Errorf("encode kick: %w", err)
	}
	endpoint := strings.TrimRight(config.RestURL, "/") +
		"/v1/pdapi/kick/" + url.PathEscape(userID)
	request, err := http.NewRequestWithContext(
		ctx,
		http.MethodPost,
		endpoint,
		bytes.NewReader(body),
	)
	if err != nil {
		return fmt.Errorf("create kick request: %w", err)
	}
	request.Header.Set("Authorization", "Bearer "+config.RestToken)
	request.Header.Set("Content-Type", "application/json; charset=utf-8")
	response, err := client.Do(request)
	if err != nil {
		return fmt.Errorf("kick player: %w", err)
	}
	defer response.Body.Close()
	_, _ = io.Copy(io.Discard, response.Body)
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf(
			"PalDefender kick returned HTTP %d",
			response.StatusCode,
		)
	}
	return nil
}
