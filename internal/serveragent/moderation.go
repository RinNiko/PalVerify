package serveragent

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strings"
)

var adminActionIDPattern = regexp.MustCompile(
	`^action_[0-9a-f]{32}$`,
)

type adminModerationAction struct {
	ActionID   string `json:"actionId"`
	Action     string `json:"action"`
	UserID     string `json:"userId"`
	PlayerName string `json:"playerName"`
	PlayerRef  string `json:"playerRef"`
	Reason     string `json:"reason"`
}

func moderationActionEndpoint(evaluateEndpoint string) string {
	const suffix = "/evaluate"
	endpoint := strings.TrimRight(strings.TrimSpace(evaluateEndpoint), "/")
	if !strings.HasSuffix(endpoint, suffix) {
		return ""
	}
	return strings.TrimSuffix(endpoint, suffix) + "/actions"
}

func validAdminModerationAction(action adminModerationAction) bool {
	return adminActionIDPattern.MatchString(action.ActionID) &&
		(action.Action == "ban" || action.Action == "unban") &&
		strings.TrimSpace(action.UserID) != "" &&
		len(action.UserID) <= 128 &&
		strings.TrimSpace(action.PlayerName) != "" &&
		len(action.PlayerName) <= 96 &&
		regexp.MustCompile(`^pv_[0-9a-f]{12}$`).MatchString(action.PlayerRef) &&
		strings.TrimSpace(action.Reason) != "" &&
		len(action.Reason) <= 180
}

func processAdminModerationAction(
	ctx context.Context,
	client *http.Client,
	config Config,
) (bool, error) {
	endpoint := moderationActionEndpoint(config.Endpoint)
	if endpoint == "" {
		return false, errors.New("moderation action endpoint is invalid")
	}
	endpoint += "?serverId=" + url.QueryEscape(config.ServerID)
	request, err := http.NewRequestWithContext(
		ctx,
		http.MethodGet,
		endpoint,
		nil,
	)
	if err != nil {
		return false, fmt.Errorf("create moderation poll: %w", err)
	}
	request.Header.Set("Accept", "application/json")
	request.Header.Set("Authorization", "Bearer "+config.ServerToken)
	response, err := client.Do(request)
	if err != nil {
		return false, fmt.Errorf("poll moderation action: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode == http.StatusNoContent {
		return false, nil
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		_, _ = io.Copy(io.Discard, response.Body)
		return false, fmt.Errorf(
			"moderation bridge returned HTTP %d",
			response.StatusCode,
		)
	}
	var document struct {
		Action adminModerationAction `json:"action"`
	}
	if err := json.NewDecoder(
		io.LimitReader(response.Body, 32*1024),
	).Decode(&document); err != nil {
		return false, fmt.Errorf("decode moderation action: %w", err)
	}
	if !validAdminModerationAction(document.Action) {
		return false, errors.New("moderation bridge returned an invalid action")
	}

	action := document.Action
	var actionErr error
	switch action.Action {
	case "ban":
		actionErr = banPlayer(
			ctx,
			client,
			config,
			action.UserID,
			action.Reason,
		)
	case "unban":
		actionErr = unbanPlayer(
			ctx,
			client,
			config,
			action.UserID,
			action.Reason,
		)
	}
	status := "completed"
	message := "PalDefender " + action.Action + " completed"
	if actionErr != nil {
		status = "failed"
		message = actionErr.Error()
	}
	if err := acknowledgeAdminModerationAction(
		ctx,
		client,
		config,
		endpoint,
		action.ActionID,
		status,
		message,
	); err != nil {
		return true, err
	}
	if actionErr != nil {
		return true, actionErr
	}
	return true, nil
}

func acknowledgeAdminModerationAction(
	ctx context.Context,
	client *http.Client,
	config Config,
	endpoint string,
	actionID string,
	status string,
	message string,
) error {
	body, err := json.Marshal(map[string]string{
		"actionId": actionID,
		"status":   status,
		"message":  message,
	})
	if err != nil {
		return fmt.Errorf("encode moderation acknowledgement: %w", err)
	}
	request, err := http.NewRequestWithContext(
		ctx,
		http.MethodPost,
		endpoint,
		bytes.NewReader(body),
	)
	if err != nil {
		return fmt.Errorf("create moderation acknowledgement: %w", err)
	}
	request.Header.Set("Accept", "application/json")
	request.Header.Set("Authorization", "Bearer "+config.ServerToken)
	request.Header.Set("Content-Type", "application/json; charset=utf-8")
	response, err := client.Do(request)
	if err != nil {
		return fmt.Errorf("acknowledge moderation action: %w", err)
	}
	defer response.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 64*1024))
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf(
			"moderation acknowledgement returned HTTP %d",
			response.StatusCode,
		)
	}
	return nil
}
