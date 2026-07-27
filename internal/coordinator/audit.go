package coordinator

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
	"sort"
	"strings"
	"sync"
	"time"
)

type AuditEvent struct {
	OccurredAt time.Time
	ServerID   string
	PlayerName string
	PlayerRef  string
	Action     string
	Reason     string
	Detail     string
	Mods       []string
	Rules      []string
}

type AuditSink interface {
	Emit(context.Context, AuditEvent)
}

type DiscordAuditSink struct {
	client     *http.Client
	webhookURL string
	logger     *log.Logger
	queue      chan AuditEvent
	mu         sync.Mutex
	recent     map[string]time.Time
}

func NewDiscordAuditSink(
	webhookURL string,
	client *http.Client,
	logger *log.Logger,
) (*DiscordAuditSink, error) {
	if err := validateDiscordWebhookURL(webhookURL); err != nil {
		return nil, err
	}
	if client == nil {
		client = &http.Client{Timeout: 8 * time.Second}
	}
	sink := &DiscordAuditSink{
		client:     client,
		webhookURL: strings.TrimSpace(webhookURL),
		logger:     logger,
		queue:      make(chan AuditEvent, 128),
		recent:     make(map[string]time.Time),
	}
	go sink.run()
	return sink, nil
}

func (sink *DiscordAuditSink) Emit(
	_ context.Context,
	event AuditEvent,
) {
	if sink == nil || sink.webhookURL == "" {
		return
	}
	key := auditEventFingerprint(event)
	occurredAt := event.OccurredAt
	if occurredAt.IsZero() {
		occurredAt = time.Now().UTC()
		event.OccurredAt = occurredAt
	}
	sink.mu.Lock()
	if sink.recent == nil {
		sink.recent = make(map[string]time.Time)
	}
	if previous, exists := sink.recent[key]; exists &&
		occurredAt.Sub(previous) < time.Minute {
		sink.mu.Unlock()
		return
	}
	sink.recent[key] = occurredAt
	for existing, seenAt := range sink.recent {
		if occurredAt.Sub(seenAt) >= 5*time.Minute {
			delete(sink.recent, existing)
		}
	}
	sink.mu.Unlock()

	select {
	case sink.queue <- event:
	default:
		sink.mu.Lock()
		delete(sink.recent, key)
		sink.mu.Unlock()
		if sink.logger != nil {
			sink.logger.Printf(
				"Discord audit queue full action=%s reason=%s",
				event.Action,
				event.Reason,
			)
		}
	}
}

func auditEventFingerprint(event AuditEvent) string {
	hash := sha256.Sum256([]byte(
		event.ServerID + "\x00" + event.PlayerRef + "\x00" +
			event.Action + "\x00" + event.Reason + "\x00" + event.Detail,
	))
	return hex.EncodeToString(hash[:])
}

func (sink *DiscordAuditSink) run() {
	for event := range sink.queue {
		ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
		err := sink.send(ctx, event)
		cancel()
		if err != nil && sink.logger != nil {
			sink.logger.Printf(
				"Discord audit delivery failed action=%s reason=%s: %v",
				event.Action,
				event.Reason,
				err,
			)
		}
	}
}

func (sink *DiscordAuditSink) send(
	ctx context.Context,
	event AuditEvent,
) error {
	fields := []map[string]any{
		{
			"name":   "Player",
			"value":  safeAuditValue(event.PlayerName, 256),
			"inline": true,
		},
		{
			"name":   "player_ref",
			"value":  safeAuditValue(event.PlayerRef, 64),
			"inline": true,
		},
		{
			"name":   "Server",
			"value":  safeAuditValue(event.ServerID, 64),
			"inline": true,
		},
		{
			"name":   "Action",
			"value":  safeAuditValue(event.Action, 128),
			"inline": true,
		},
		{
			"name":   "Reason",
			"value":  safeAuditValue(event.Reason, 256),
			"inline": true,
		},
	}
	appendField := func(name string, value string) {
		value = safeAuditValue(value, 1024)
		if value == "" {
			return
		}
		fields = append(fields, map[string]any{
			"name":   name,
			"value":  value,
			"inline": false,
		})
	}
	appendField("Integrity rules", strings.Join(event.Rules, "\n"))
	appendField("Rejected mods", strings.Join(event.Mods, "\n"))
	appendField("Diagnostic detail", event.Detail)

	payload := map[string]any{
		"username": "PalVerify Coordinator",
		"allowed_mentions": map[string]any{
			"parse": []string{},
		},
		"embeds": []map[string]any{{
			"title":     "PalVerify diagnostic",
			"color":     0xe74c3c,
			"timestamp": event.OccurredAt.UTC().Format(time.RFC3339Nano),
			"fields":    fields,
		}},
	}
	body, err := json.Marshal(payload)
	if err != nil {
		return fmt.Errorf("encode Discord audit: %w", err)
	}
	request, err := http.NewRequestWithContext(
		ctx,
		http.MethodPost,
		sink.webhookURL,
		bytes.NewReader(body),
	)
	if err != nil {
		return errors.New("create Discord audit request failed")
	}
	request.Header.Set("Content-Type", "application/json; charset=utf-8")
	response, err := sink.client.Do(request)
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

func auditPlayerReference(secret string, userID string) string {
	userID = strings.TrimSpace(userID)
	if userID == "" {
		return "unavailable"
	}
	mac := hmac.New(sha256.New, []byte(secret))
	_, _ = mac.Write([]byte(userID))
	return "pv_" + hex.EncodeToString(mac.Sum(nil))[:12]
}

func safeAuditPlayerName(value string) string {
	value = safeAuditValue(value, 96)
	if value == "" {
		return "unknown"
	}
	return value
}

func safeAuditValue(value string, maximum int) string {
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

func auditRules(reason string, detail string) []string {
	if reason != "INTEGRITY_VIOLATION" {
		return nil
	}
	values := strings.Split(detail, ",")
	rules := make([]string, 0, len(values))
	seen := make(map[string]struct{}, len(values))
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

func auditRulesWithEvidence(
	reason string,
	violations []string,
	detail string,
) []string {
	if reason == "INTEGRITY_VIOLATION" && len(violations) != 0 {
		return append([]string(nil), violations...)
	}
	return auditRules(reason, detail)
}
