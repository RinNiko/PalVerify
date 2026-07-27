package serveragent

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
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
	"sort"
	"strconv"
	"strings"
	"time"
)

const temporaryIntegrityBanDuration = 24 * time.Hour

type disciplineRecord struct {
	IntegrityOffenses int       `json:"integrityOffenses"`
	TemporaryBanUntil time.Time `json:"temporaryBanUntil,omitempty"`
	Permanent         bool      `json:"permanent"`
	LastPlayerName    string    `json:"lastPlayerName,omitempty"`
	LastIP            string    `json:"lastIp,omitempty"`
	LastRules         []string  `json:"lastRules,omitempty"`
	UpdatedAt         time.Time `json:"updatedAt"`
}

type connectedPlayer struct {
	Name string `json:"name"`
	IP   string `json:"ip,omitempty"`
}

type discordAuditEvent struct {
	EventID    string    `json:"eventId"`
	OccurredAt time.Time `json:"occurredAt"`
	ServerID   string    `json:"serverId"`
	PlayerName string    `json:"playerName"`
	PlayerRef  string    `json:"playerRef"`
	UserID     string    `json:"userId"`
	IP         string    `json:"ip,omitempty"`
	Action     string    `json:"action"`
	Reason     string    `json:"reason"`
	Detail     string    `json:"detail,omitempty"`
	Mods       []string  `json:"mods,omitempty"`
	Rules      []string  `json:"rules,omitempty"`
	Discipline string    `json:"discipline,omitempty"`
	ExpiresAt  time.Time `json:"expiresAt,omitempty"`
	Offense    int       `json:"offense,omitempty"`
	Source     string    `json:"source,omitempty"`
}

type disciplineState struct {
	Version              int                         `json:"version"`
	Players              map[string]disciplineRecord `json:"players"`
	PendingAudits        []discordAuditEvent         `json:"pendingAudits,omitempty"`
	LogCursors           map[string]int64            `json:"palDefenderLogCursors,omitempty"`
	ConnectedPlayers     map[string]connectedPlayer  `json:"connectedPlayers,omitempty"`
	ActivePalDefenderLog string                      `json:"activePalDefenderLog,omitempty"`
	palDefenderChanged   bool
}

func loadDisciplineState(path string) (disciplineState, error) {
	state := disciplineState{
		Version:          1,
		Players:          make(map[string]disciplineRecord),
		LogCursors:       make(map[string]int64),
		ConnectedPlayers: make(map[string]connectedPlayer),
	}
	if strings.TrimSpace(path) == "" {
		return state, nil
	}
	body, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return state, nil
	}
	if err != nil {
		return disciplineState{}, fmt.Errorf("read discipline state: %w", err)
	}
	if err := json.Unmarshal(body, &state); err != nil {
		return disciplineState{}, fmt.Errorf("decode discipline state: %w", err)
	}
	if state.Version != 1 {
		return disciplineState{}, fmt.Errorf(
			"unsupported discipline state version %d",
			state.Version,
		)
	}
	if state.Players == nil {
		state.Players = make(map[string]disciplineRecord)
	}
	if state.LogCursors == nil {
		state.LogCursors = make(map[string]int64)
	}
	if state.ConnectedPlayers == nil {
		state.ConnectedPlayers = make(map[string]connectedPlayer)
	}
	return state, nil
}

func saveDisciplineState(path string, state disciplineState) error {
	if strings.TrimSpace(path) == "" {
		return errors.New("discipline state path is required")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return fmt.Errorf("create discipline state directory: %w", err)
	}
	body, err := json.MarshalIndent(state, "", "  ")
	if err != nil {
		return fmt.Errorf("encode discipline state: %w", err)
	}
	body = append(body, '\n')
	temporary := path + ".tmp"
	if err := os.WriteFile(temporary, body, 0o600); err != nil {
		return fmt.Errorf("write discipline state: %w", err)
	}
	if err := os.Rename(temporary, path); err == nil {
		return nil
	}
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("replace discipline state: %w", err)
	}
	if err := os.Rename(temporary, path); err != nil {
		return fmt.Errorf("replace discipline state: %w", err)
	}
	return nil
}

func processExpiredTemporaryBans(
	ctx context.Context,
	client *http.Client,
	config Config,
	now time.Time,
	state *disciplineState,
	logger *log.Logger,
) error {
	if strings.TrimSpace(config.DisciplineStatePath) == "" {
		return nil
	}
	userIDs := make([]string, 0, len(state.Players))
	for userID := range state.Players {
		userIDs = append(userIDs, userID)
	}
	sort.Strings(userIDs)

	changed := false
	for _, userID := range userIDs {
		record := state.Players[userID]
		if record.Permanent ||
			record.TemporaryBanUntil.IsZero() ||
			now.Before(record.TemporaryBanUntil) {
			continue
		}
		if err := unbanPlayer(
			ctx,
			client,
			config,
			userID,
			"PalVerify: 24-hour integrity suspension expired.",
		); err != nil {
			return err
		}
		record.TemporaryBanUntil = time.Time{}
		record.UpdatedAt = now.UTC()
		state.Players[userID] = record
		changed = true
		if logger != nil {
			logger.Printf(
				"[PalVerify] temporary integrity ban expired player=%s ref=%s",
				safePlayerName(record.LastPlayerName),
				playerReference(config.ServerToken, userID),
			)
		}
		queueDiscordAudit(config, state, discordAuditEvent{
			OccurredAt: now.UTC(),
			ServerID:   config.ServerID,
			PlayerName: safePlayerName(record.LastPlayerName),
			PlayerRef:  playerReference(config.ServerToken, userID),
			UserID:     userID,
			IP:         record.LastIP,
			Action:     "TEMPORARY_BAN_EXPIRED",
			Reason:     "INTEGRITY_VIOLATION",
			Rules:      append([]string(nil), record.LastRules...),
			Discipline: "unbanned after 24 hours",
			Offense:    record.IntegrityOffenses,
		})
	}
	if changed {
		return saveDisciplineState(config.DisciplineStatePath, *state)
	}
	return nil
}

func enforceIntegrityDiscipline(
	ctx context.Context,
	client *http.Client,
	config Config,
	now time.Time,
	result decision,
	state *disciplineState,
	logger *log.Logger,
) (bool, error) {
	if strings.TrimSpace(config.DisciplineStatePath) == "" {
		return false, errors.New(
			"discipline state path is required for integrity enforcement",
		)
	}

	record := state.Players[result.UserID]
	if record.Permanent || now.Before(record.TemporaryBanUntil) {
		return false, nil
	}

	rules := integrityRules(result)
	record.IntegrityOffenses++
	record.LastPlayerName = safePlayerName(result.Name)
	record.LastIP = strings.TrimSpace(result.IP)
	record.LastRules = append([]string(nil), rules...)
	record.UpdatedAt = now.UTC()

	permanent := record.IntegrityOffenses > 1
	reason := formatIntegrityBanReason(permanent, rules)
	if err := banPlayer(
		ctx,
		client,
		config,
		result.UserID,
		reason,
	); err != nil {
		return false, err
	}

	discipline := "24-hour ban"
	if permanent {
		record.Permanent = true
		record.TemporaryBanUntil = time.Time{}
		discipline = "permanent ban"
	} else {
		record.TemporaryBanUntil =
			now.UTC().Add(temporaryIntegrityBanDuration)
	}
	state.Players[result.UserID] = record
	queueDiscordAudit(config, state, discordAuditEvent{
		OccurredAt: now.UTC(),
		ServerID:   config.ServerID,
		PlayerName: record.LastPlayerName,
		PlayerRef:  playerReference(config.ServerToken, result.UserID),
		UserID:     result.UserID,
		IP:         record.LastIP,
		Action:     "INTEGRITY_BAN",
		Reason:     result.Reason,
		Detail:     result.Detail,
		Rules:      rules,
		Discipline: discipline,
		ExpiresAt:  record.TemporaryBanUntil,
		Offense:    record.IntegrityOffenses,
	})
	if err := saveDisciplineState(config.DisciplineStatePath, *state); err != nil {
		return false, err
	}
	if logger != nil {
		logger.Printf(
			"[PalVerify] integrity ban player=%s ref=%s offense=%d discipline=%s rules=%s",
			record.LastPlayerName,
			playerReference(config.ServerToken, result.UserID),
			record.IntegrityOffenses,
			discipline,
			strings.Join(rules, ","),
		)
	}
	return true, nil
}

func queueKickAudit(
	config Config,
	state *disciplineState,
	now time.Time,
	result decision,
) {
	queueDiscordAudit(config, state, discordAuditEvent{
		OccurredAt: now.UTC(),
		ServerID:   config.ServerID,
		PlayerName: safePlayerName(result.Name),
		PlayerRef:  playerReference(config.ServerToken, result.UserID),
		UserID:     result.UserID,
		IP:         strings.TrimSpace(result.IP),
		Action:     "KICK",
		Reason:     result.Reason,
		Detail:     result.Detail,
		Mods:       append([]string(nil), result.Mods...),
		Rules:      integrityRules(result),
	})
}

func queueDiscordAudit(
	config Config,
	state *disciplineState,
	event discordAuditEvent,
) {
	if strings.TrimSpace(config.DiscordWebhookURL) == "" &&
		adminAuditEndpoint(config.Endpoint) == "" {
		return
	}
	event.EventID = discordEventID(event)
	state.PendingAudits = append(state.PendingAudits, event)
}

func flushDiscordAudits(
	ctx context.Context,
	client *http.Client,
	config Config,
	state *disciplineState,
	logger *log.Logger,
) {
	hasDestination := strings.TrimSpace(config.DiscordWebhookURL) != "" ||
		adminAuditEndpoint(config.Endpoint) != ""
	if !hasDestination || len(state.PendingAudits) == 0 {
		return
	}
	sent := 0
	for _, event := range state.PendingAudits {
		if endpoint := adminAuditEndpoint(config.Endpoint); endpoint != "" {
			if err := sendAdminAudit(
				ctx,
				client,
				endpoint,
				config.ServerToken,
				event,
			); err != nil {
				if logger != nil {
					logger.Printf(
						"[PalVerify] admin audit delivery failed event=%s: %v",
						event.EventID,
						err,
					)
				}
				break
			}
		}
		if strings.TrimSpace(config.DiscordWebhookURL) == "" {
			sent++
			continue
		}
		if err := sendDiscordAudit(
			ctx,
			client,
			config.DiscordWebhookURL,
			event,
		); err != nil {
			if logger != nil {
				logger.Printf(
					"[PalVerify] Discord audit delivery failed event=%s: %v",
					event.EventID,
					err,
				)
			}
			break
		}
		sent++
	}
	if sent == 0 {
		return
	}
	state.PendingAudits = append(
		[]discordAuditEvent(nil),
		state.PendingAudits[sent:]...,
	)
	if strings.TrimSpace(config.DisciplineStatePath) != "" {
		if err := saveDisciplineState(
			config.DisciplineStatePath,
			*state,
		); err != nil && logger != nil {
			logger.Printf(
				"[PalVerify] audit outbox save failed: %v",
				err,
			)
		}
	}
}

func adminAuditEndpoint(evaluateEndpoint string) string {
	parsed, err := url.Parse(strings.TrimSpace(evaluateEndpoint))
	if err != nil || parsed.Hostname() == "" {
		return ""
	}
	path := strings.TrimSuffix(parsed.EscapedPath(), "/")
	if !strings.HasSuffix(path, "/evaluate") {
		return ""
	}
	parsed.Path = strings.TrimSuffix(parsed.Path, "/evaluate") + "/audit"
	parsed.RawPath = ""
	parsed.RawQuery = ""
	parsed.Fragment = ""
	return parsed.String()
}

func sendAdminAudit(
	ctx context.Context,
	client *http.Client,
	endpoint string,
	serverToken string,
	event discordAuditEvent,
) error {
	payload := map[string]any{
		"eventId":    event.EventID,
		"occurredAt": event.OccurredAt.UTC().Format(time.RFC3339Nano),
		"serverId":   event.ServerID,
		"playerName": event.PlayerName,
		"playerRef":  event.PlayerRef,
		"userId":     event.UserID,
		"action":     event.Action,
		"reason":     event.Reason,
	}
	if event.IP != "" {
		payload["ip"] = event.IP
	}
	if event.Detail != "" {
		payload["detail"] = event.Detail
	}
	if len(event.Mods) != 0 {
		payload["mods"] = event.Mods
	}
	if len(event.Rules) != 0 {
		payload["rules"] = event.Rules
	}
	if event.Discipline != "" {
		payload["discipline"] = event.Discipline
	}
	if !event.ExpiresAt.IsZero() {
		payload["expiresAt"] = event.ExpiresAt.UTC().Format(time.RFC3339Nano)
	}
	if event.Offense != 0 {
		payload["offense"] = event.Offense
	}
	body, err := json.Marshal(payload)
	if err != nil {
		return fmt.Errorf("encode admin audit: %w", err)
	}
	request, err := http.NewRequestWithContext(
		ctx,
		http.MethodPost,
		endpoint,
		bytes.NewReader(body),
	)
	if err != nil {
		return errors.New("create admin audit request failed")
	}
	request.Header.Set("Authorization", "Bearer "+serverToken)
	request.Header.Set("Content-Type", "application/json; charset=utf-8")
	response, err := client.Do(request)
	if err != nil {
		return errors.New("admin audit request failed")
	}
	defer response.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 64*1024))
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf(
			"admin audit returned HTTP %d",
			response.StatusCode,
		)
	}
	return nil
}

func sendDiscordAudit(
	ctx context.Context,
	client *http.Client,
	webhookURL string,
	event discordAuditEvent,
) error {
	fields := []map[string]any{
		{
			"name":   "Player",
			"value":  safeDiscordValue(event.PlayerName, 256),
			"inline": true,
		},
		{
			"name":   "player_ref",
			"value":  safeDiscordValue(event.PlayerRef, 64),
			"inline": true,
		},
		{
			"name":   "Server",
			"value":  safeDiscordValue(event.ServerID, 64),
			"inline": true,
		},
		{
			"name":   "Action",
			"value":  safeDiscordValue(event.Action, 128),
			"inline": true,
		},
		{
			"name":   "Reason",
			"value":  safeDiscordValue(event.Reason, 256),
			"inline": true,
		},
	}
	appendField := func(name string, value string, inline bool) {
		value = safeDiscordValue(value, 1024)
		if value == "" {
			return
		}
		fields = append(fields, map[string]any{
			"name":   name,
			"value":  value,
			"inline": inline,
		})
	}
	appendField("Integrity rules", strings.Join(event.Rules, "\n"), false)
	appendField("Rejected mods", strings.Join(event.Mods, "\n"), false)
	appendField("Diagnostic detail", event.Detail, false)
	appendField("Discipline", event.Discipline, true)
	if event.Offense != 0 {
		appendField("Integrity offense", strconv.Itoa(event.Offense), true)
	}
	if !event.ExpiresAt.IsZero() {
		appendField(
			"Expires (UTC)",
			event.ExpiresAt.UTC().Format(time.RFC3339),
			true,
		)
	}

	color := 0xf1c40f
	switch event.Action {
	case "INTEGRITY_BAN":
		if event.Discipline == "permanent ban" {
			color = 0x992d22
		} else {
			color = 0xe67e22
		}
	case "TEMPORARY_BAN_EXPIRED":
		color = 0x2ecc71
	}
	payload := map[string]any{
		"username": "PalVerify Audit",
		"allowed_mentions": map[string]any{
			"parse": []string{},
		},
		"embeds": []map[string]any{{
			"title":       "PalVerify enforcement",
			"description": safeDiscordValue(event.EventID, 128),
			"color":       color,
			"timestamp": event.OccurredAt.UTC().Format(
				time.RFC3339Nano,
			),
			"fields": fields,
		}},
	}
	body, err := json.Marshal(payload)
	if err != nil {
		return fmt.Errorf("encode Discord audit: %w", err)
	}
	request, err := http.NewRequestWithContext(
		ctx,
		http.MethodPost,
		webhookURL,
		bytes.NewReader(body),
	)
	if err != nil {
		return errors.New("create Discord audit request failed")
	}
	request.Header.Set("Content-Type", "application/json; charset=utf-8")
	response, err := client.Do(request)
	if err != nil {
		return errors.New("Discord webhook request failed")
	}
	defer response.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 64*1024))
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf(
			"Discord webhook returned HTTP %d",
			response.StatusCode,
		)
	}
	return nil
}

func validateDiscordWebhookURL(value string) error {
	value = strings.TrimSpace(value)
	if value == "" {
		return nil
	}
	parsed, err := url.Parse(value)
	if err != nil || parsed.Hostname() == "" {
		return errors.New("Discord webhook URL is invalid")
	}
	host := strings.ToLower(parsed.Hostname())
	if parsed.Scheme != "https" ||
		(host != "discord.com" && !strings.HasSuffix(host, ".discord.com")) ||
		!strings.HasPrefix(parsed.EscapedPath(), "/api/webhooks/") {
		return errors.New(
			"Discord webhook URL must use the Discord HTTPS webhook endpoint",
		)
	}
	return nil
}

func playerReference(secret string, userID string) string {
	mac := hmac.New(sha256.New, []byte(secret))
	_, _ = mac.Write([]byte(userID))
	return "pv_" + hex.EncodeToString(mac.Sum(nil))[:12]
}

func discordEventID(event discordAuditEvent) string {
	hash := sha256.Sum256([]byte(
		event.OccurredAt.UTC().Format(time.RFC3339Nano) + "\x00" +
			event.ServerID + "\x00" + event.PlayerRef + "\x00" +
			event.Action + "\x00" + event.Reason,
	))
	return "event_" + hex.EncodeToString(hash[:])[:16]
}

func safePlayerName(value string) string {
	value = safeDiscordValue(value, 96)
	if value == "" {
		return "unknown"
	}
	return value
}

func safeDiscordValue(value string, maximum int) string {
	value = strings.TrimSpace(value)
	value = strings.Map(func(character rune) rune {
		if character < 0x20 || character == 0x7f {
			return ' '
		}
		return character
	}, value)
	value = strings.ReplaceAll(value, "@", "(at)")
	runes := []rune(value)
	if len(runes) > maximum {
		value = string(runes[:maximum])
	}
	return strings.TrimSpace(value)
}

func integrityRules(result decision) []string {
	values := result.Violations
	if len(values) == 0 && result.Reason == "INTEGRITY_VIOLATION" {
		values = strings.Split(result.Detail, ",")
	}
	seen := make(map[string]struct{}, len(values))
	rules := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value == "" {
			continue
		}
		if _, exists := seen[value]; exists {
			continue
		}
		seen[value] = struct{}{}
		rules = append(rules, value)
	}
	sort.Strings(rules)
	return rules
}

func formatIntegrityBanReason(permanent bool, rules []string) string {
	prefix := "PalVerify: 24-hour integrity suspension"
	if permanent {
		prefix = "PalVerify: repeated integrity violation; permanent ban"
	}
	if len(rules) != 0 {
		prefix += " (" + strings.Join(rules, ",") + ")"
	}
	if len(prefix) > 180 {
		prefix = prefix[:180]
	}
	return prefix
}

func banPlayer(
	ctx context.Context,
	client *http.Client,
	config Config,
	userID string,
	reason string,
) error {
	body, err := json.Marshal(map[string]any{
		"Reason": reason,
		"IP":     false,
	})
	if err != nil {
		return fmt.Errorf("encode ban: %w", err)
	}
	return postPunishment(
		ctx,
		client,
		config,
		"/v1/pdapi/ban/"+url.PathEscape(userID),
		body,
		"ban",
		false,
	)
}

func unbanPlayer(
	ctx context.Context,
	client *http.Client,
	config Config,
	userID string,
	reason string,
) error {
	body, err := json.Marshal(map[string]string{"Reason": reason})
	if err != nil {
		return fmt.Errorf("encode unban: %w", err)
	}
	return postPunishment(
		ctx,
		client,
		config,
		"/v1/pdapi/unban/"+url.PathEscape(userID),
		body,
		"unban",
		true,
	)
}

func postPunishment(
	ctx context.Context,
	client *http.Client,
	config Config,
	path string,
	body []byte,
	action string,
	notFoundIsSuccess bool,
) error {
	endpoint := strings.TrimRight(config.RestURL, "/") + path
	request, err := http.NewRequestWithContext(
		ctx,
		http.MethodPost,
		endpoint,
		bytes.NewReader(body),
	)
	if err != nil {
		return fmt.Errorf("create %s request: %w", action, err)
	}
	request.Header.Set("Authorization", "Bearer "+config.RestToken)
	request.Header.Set("Content-Type", "application/json; charset=utf-8")
	response, err := client.Do(request)
	if err != nil {
		return fmt.Errorf("%s player: %w", action, err)
	}
	defer response.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 64*1024))
	if notFoundIsSuccess && response.StatusCode == http.StatusNotFound {
		return nil
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf(
			"PalDefender %s returned HTTP %d",
			action,
			response.StatusCode,
		)
	}
	return nil
}
