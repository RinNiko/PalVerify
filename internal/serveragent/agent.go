package serveragent

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
)

type Config struct {
	Endpoint        string `json:"endpoint"`
	ServerToken     string `json:"serverToken"`
	ServerID        string `json:"serverId"`
	RestURL         string `json:"restUrl"`
	RestToken       string `json:"restToken"`
	IntervalSeconds int    `json:"intervalSeconds"`
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
	if config.IntervalSeconds < 5 || config.IntervalSeconds > 60 {
		return errors.New("intervalSeconds must be between 5 and 60")
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
	UserID string   `json:"userId"`
	Name   string   `json:"name"`
	Action string   `json:"action"`
	Reason string   `json:"reason"`
	Mods   []string `json:"mods"`
}

type evaluateResponse struct {
	Decisions []decision `json:"decisions"`
}

func SyncOnce(
	ctx context.Context,
	client *http.Client,
	config Config,
	now time.Time,
) (int, error) {
	document, err := fetchPlayers(ctx, client, config)
	if err != nil {
		return 0, err
	}

	online := make([]onlinePlayer, 0, len(document.Players))
	for _, candidate := range document.Players {
		if !strings.EqualFold(strings.TrimSpace(candidate.Status), "online") {
			continue
		}
		userID := strings.TrimSpace(candidate.UserID)
		name := strings.TrimSpace(candidate.Name)
		if userID == "" || name == "" {
			continue
		}
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
		if result.Action != "kick" {
			continue
		}
		reason := "PalVerify: " + result.Reason
		if len(result.Mods) != 0 {
			reason += " (" + strings.Join(result.Mods, ",") + ")"
		}
		if len(reason) > 180 {
			reason = reason[:180]
		}
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
	}
	return kicks, nil
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
