package serveragent

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

const maximumPalDefenderLogReadBytes = 8 * 1024 * 1024

type palDefenderIdentity struct {
	PlayerName string
	IP         string
}

var (
	palDefenderIdentityPattern = regexp.MustCompile(
		`'([^']{1,96})' \(UserId=([A-Za-z0-9_-]{3,128}), IP=([0-9a-fA-F:.]{3,64})\) has logged (in|out)(?: with auto admin mode)?\.`,
	)
	palDefenderBanPattern = regexp.MustCompile(
		`Banned UserId='([A-Za-z0-9_-]{3,128})' UserIP='([^']*)'.* reason='([^']{1,180})'`,
	)
	palDefenderUnbanPattern = regexp.MustCompile(
		`Unbanned UserId='([A-Za-z0-9_-]{3,128})'.* reason='([^']{1,180})'`,
	)
	palDefenderCheatPattern = regexp.MustCompile(
		`'([^']{1,96})' \(UserId=([A-Za-z0-9_-]{3,128}), IP=([0-9a-fA-F:.]{3,64})\) \*may be\* a cheater! Reason: (.{1,512}?) Not taking any actions`,
	)
	palDefenderDamageThresholdPattern = regexp.MustCompile(
		`RequestDamageMapObject_ToServer: '([^']{1,96})' \(UserId=([A-Za-z0-9_-]{3,128}), IP=([0-9a-fA-F:.]{3,64})\) have tried to deal more damage \(([0-9]+(?:\.[0-9]+)?)\) than pvpMaxBuildingDamage threshold \(([0-9]+(?:\.[0-9]+)?)\)`,
	)
	palDefenderGroupAccessPattern = regexp.MustCompile(
		`'([^']{1,96})' \(UserId=([A-Za-z0-9_-]{3,128}), IP=([0-9a-fA-F:.]{3,64})\) CanInteractWithGroupID failure\.`,
	)
	palDefenderIllegalAccessPattern = regexp.MustCompile(
		`Illegal access attempt from ''?([^']{1,96})' \(UserId=([A-Za-z0-9_-]{3,128}), IP=([0-9a-fA-F:.]{3,64})\)`,
	)
	palDefenderMissingAttackerPattern = regexp.MustCompile(
		`'([^']{1,96})' \(UserId=([A-Za-z0-9_-]{3,128}), IP=([0-9a-fA-F:.]{3,64})\) attempted to deal damage without Attacker\?`,
	)
)

func parsePalDefenderLogLine(
	line string,
	identities map[string]palDefenderIdentity,
	serverID string,
	serverToken string,
	occurredAt time.Time,
	eventID string,
) *discordAuditEvent {
	line = strings.TrimSpace(line)
	if line == "" {
		return nil
	}
	if matched := palDefenderIdentityPattern.FindStringSubmatch(line); matched != nil {
		identity := palDefenderIdentity{
			PlayerName: safePlayerName(matched[1]),
			IP:         validAuditIP(matched[3]),
		}
		identities[matched[2]] = identity
		return nil
	}
	if matched := palDefenderCheatPattern.FindStringSubmatch(line); matched != nil {
		ip := validAuditIP(matched[3])
		identities[matched[2]] = palDefenderIdentity{
			PlayerName: safePlayerName(matched[1]),
			IP:         ip,
		}
		return &discordAuditEvent{
			EventID:    eventID,
			OccurredAt: occurredAt.UTC(),
			ServerID:   serverID,
			PlayerName: safePlayerName(matched[1]),
			PlayerRef:  playerReference(serverToken, matched[2]),
			UserID:     matched[2],
			IP:         ip,
			Action:     "PALDEFENDER_ALERT",
			Reason:     palDefenderCheatReason(matched[4]),
			Detail:     safeDiscordValue(matched[4], 512),
			Source:     "paldefender",
		}
	}
	if matched := palDefenderDamageThresholdPattern.FindStringSubmatch(line); matched != nil {
		return palDefenderAlert(
			matched[1],
			matched[2],
			matched[3],
			"DAMAGE_THRESHOLD_EXCEEDED",
			fmt.Sprintf(
				"Attempted building damage %s exceeded threshold %s.",
				matched[4],
				matched[5],
			),
			identities,
			serverID,
			serverToken,
			occurredAt,
			eventID,
		)
	}
	if matched := palDefenderGroupAccessPattern.FindStringSubmatch(line); matched != nil {
		return palDefenderAlert(
			matched[1],
			matched[2],
			matched[3],
			"GROUP_ACCESS_REJECTED",
			"Container guild interaction was rejected.",
			identities,
			serverID,
			serverToken,
			occurredAt,
			eventID,
		)
	}
	if matched := palDefenderIllegalAccessPattern.FindStringSubmatch(line); matched != nil {
		return palDefenderAlert(
			matched[1],
			matched[2],
			matched[3],
			"ILLEGAL_GROUP_ACCESS",
			"Illegal guild or container access attempt.",
			identities,
			serverID,
			serverToken,
			occurredAt,
			eventID,
		)
	}
	if matched := palDefenderMissingAttackerPattern.FindStringSubmatch(line); matched != nil {
		return palDefenderAlert(
			matched[1],
			matched[2],
			matched[3],
			"MISSING_DAMAGE_ATTACKER",
			"Attempted damage without an attacker.",
			identities,
			serverID,
			serverToken,
			occurredAt,
			eventID,
		)
	}
	if matched := palDefenderBanPattern.FindStringSubmatch(line); matched != nil {
		identity := identities[matched[1]]
		ip := identity.IP
		if candidate := validAuditIP(matched[2]); candidate != "" {
			ip = candidate
		}
		return &discordAuditEvent{
			EventID:    eventID,
			OccurredAt: occurredAt.UTC(),
			ServerID:   serverID,
			PlayerName: safePlayerName(identity.PlayerName),
			PlayerRef:  playerReference(serverToken, matched[1]),
			UserID:     matched[1],
			IP:         ip,
			Action:     "PALDEFENDER_BAN",
			Reason:     "PALDEFENDER_PUNISHMENT",
			Detail:     safeDiscordValue(matched[3], 180),
			Discipline: "ban",
			Source:     "paldefender",
		}
	}
	if matched := palDefenderUnbanPattern.FindStringSubmatch(line); matched != nil {
		identity := identities[matched[1]]
		return &discordAuditEvent{
			EventID:    eventID,
			OccurredAt: occurredAt.UTC(),
			ServerID:   serverID,
			PlayerName: safePlayerName(identity.PlayerName),
			PlayerRef:  playerReference(serverToken, matched[1]),
			UserID:     matched[1],
			IP:         identity.IP,
			Action:     "PALDEFENDER_UNBAN",
			Reason:     "PALDEFENDER_PUNISHMENT",
			Detail:     safeDiscordValue(matched[2], 180),
			Discipline: "unban",
			Source:     "paldefender",
		}
	}
	return nil
}

func palDefenderAlert(
	playerName string,
	userID string,
	ipValue string,
	reason string,
	detail string,
	identities map[string]palDefenderIdentity,
	serverID string,
	serverToken string,
	occurredAt time.Time,
	eventID string,
) *discordAuditEvent {
	ip := validAuditIP(ipValue)
	playerName = safePlayerName(playerName)
	identities[userID] = palDefenderIdentity{
		PlayerName: playerName,
		IP:         ip,
	}
	return &discordAuditEvent{
		EventID:    eventID,
		OccurredAt: occurredAt.UTC(),
		ServerID:   serverID,
		PlayerName: playerName,
		PlayerRef:  playerReference(serverToken, userID),
		UserID:     userID,
		IP:         ip,
		Action:     "PALDEFENDER_ALERT",
		Reason:     reason,
		Detail:     safeDiscordValue(detail, 512),
		Source:     "paldefender",
	}
}

func palDefenderCheatReason(detail string) string {
	detail = strings.ToLower(detail)
	switch {
	case strings.Contains(detail, "requestunlockfasttravelpoint"):
		return "FAST_TRAVEL_DISTANCE"
	case strings.Contains(detail, "pickup droppedcharacter"):
		return "REMOTE_CHARACTER_PICKUP"
	case strings.Contains(detail, "pickup object"):
		return "REMOTE_PICKUP"
	case strings.Contains(detail, "requestmoveitemtoinventoryfromcontainer"):
		return "CONTAINER_ACCESS_REJECTED"
	case strings.Contains(detail, "pal sphere") &&
		strings.Contains(detail, "inventory"):
		return "INVALID_SPHERE_SELECTION"
	default:
		return "HUMAN_REVIEW_REQUIRED"
	}
}

func validAuditIP(value string) string {
	value = strings.TrimSpace(value)
	if net.ParseIP(value) == nil || net.ParseIP(value).IsLoopback() {
		return ""
	}
	return value
}

func collectPalDefenderAudits(
	config Config,
	state *disciplineState,
	now time.Time,
) (int, error) {
	directory := strings.TrimSpace(config.PalDefenderLogDirectory)
	if directory == "" {
		return 0, nil
	}
	if state.LogCursors == nil {
		state.LogCursors = make(map[string]int64)
	}
	if state.ConnectedPlayers == nil {
		state.ConnectedPlayers = make(map[string]connectedPlayer)
	}
	paths := make([]string, 0, 2)
	rootLogPath := ""
	for index, pattern := range []string{
		filepath.Join(directory, "*.log"),
		filepath.Join(directory, "Cheats", "*.log"),
	} {
		matches, err := filepath.Glob(pattern)
		if err != nil {
			return 0, fmt.Errorf("find PalDefender logs: %w", err)
		}
		if latest := latestLogPath(matches); latest != "" {
			paths = append(paths, latest)
			if index == 0 {
				rootLogPath = latest
			}
		}
	}
	if len(paths) == 0 {
		return 0, nil
	}

	identities := make(map[string]palDefenderIdentity)
	for userID, record := range state.Players {
		identities[userID] = palDefenderIdentity{
			PlayerName: record.LastPlayerName,
			IP:         validAuditIP(record.LastIP),
		}
	}
	collected := 0
	activeCursors := make(map[string]int64, len(paths))
	type aggregate struct {
		index       int
		count       int
		firstDetail string
	}
	aggregates := make(map[string]*aggregate)
	for _, path := range paths {
		body, err := os.ReadFile(path)
		if err != nil {
			return collected, fmt.Errorf("read PalDefender log: %w", err)
		}
		size := int64(len(body))
		start := state.LogCursors[path]
		if path == rootLogPath &&
			state.ActivePalDefenderLog != rootLogPath {
			state.ConnectedPlayers = make(map[string]connectedPlayer)
			state.ActivePalDefenderLog = rootLogPath
			start = 0
			state.palDefenderChanged = true
		}
		if start < 0 || start > size {
			if path == rootLogPath {
				state.ConnectedPlayers = make(map[string]connectedPlayer)
				state.palDefenderChanged = true
			}
			start = 0
		}
		if start == 0 && size > maximumPalDefenderLogReadBytes {
			start = size - maximumPalDefenderLogReadBytes
		}
		segment := body[start:]
		if start > 0 {
			if newline := strings.IndexByte(string(segment), '\n'); newline >= 0 {
				start += int64(newline + 1)
				segment = body[start:]
			}
		}
		lines := strings.Split(string(segment), "\n")
		offset := start
		for _, line := range lines {
			lineBytes := int64(len(line) + 1)
			if path == rootLogPath &&
				updateConnectedPlayer(line, state.ConnectedPlayers) {
				state.palDefenderChanged = true
			}
			eventID := palDefenderLogEventID(path, offset, line)
			if event := parsePalDefenderLogLine(
				line,
				identities,
				config.ServerID,
				config.ServerToken,
				now,
				eventID,
			); event != nil {
				if event.Action == "PALDEFENDER_ALERT" {
					key := event.PlayerRef + "\x00" + event.Reason
					if current := aggregates[key]; current != nil {
						current.count++
						offset += lineBytes
						continue
					}
					state.PendingAudits = append(state.PendingAudits, *event)
					aggregates[key] = &aggregate{
						index:       len(state.PendingAudits) - 1,
						count:       1,
						firstDetail: event.Detail,
					}
					collected++
				} else {
					state.PendingAudits = append(state.PendingAudits, *event)
					collected++
				}
			}
			offset += lineBytes
		}
		activeCursors[path] = size
		if state.LogCursors[path] != size {
			state.palDefenderChanged = true
		}
	}
	for _, current := range aggregates {
		if current.count <= 1 {
			continue
		}
		state.PendingAudits[current.index].Detail = safeDiscordValue(
			fmt.Sprintf(
				"%s occurrences=%d",
				current.firstDetail,
				current.count,
			),
			512,
		)
	}
	state.LogCursors = activeCursors
	return collected, nil
}

func updateConnectedPlayer(
	line string,
	connected map[string]connectedPlayer,
) bool {
	matched := palDefenderIdentityPattern.FindStringSubmatch(
		strings.TrimSpace(line),
	)
	if matched == nil {
		return false
	}
	userID := matched[2]
	if matched[4] == "out" {
		if _, exists := connected[userID]; !exists {
			return false
		}
		delete(connected, userID)
		return true
	}
	next := connectedPlayer{
		Name: safePlayerName(matched[1]),
		IP:   validAuditIP(matched[3]),
	}
	if current, exists := connected[userID]; exists && current == next {
		return false
	}
	connected[userID] = next
	return true
}

func latestLogPath(paths []string) string {
	type candidate struct {
		path    string
		modTime time.Time
	}
	candidates := make([]candidate, 0, len(paths))
	for _, path := range paths {
		info, err := os.Stat(path)
		if err == nil && !info.IsDir() {
			candidates = append(candidates, candidate{
				path:    path,
				modTime: info.ModTime(),
			})
		}
	}
	sort.Slice(candidates, func(left, right int) bool {
		return candidates[left].modTime.Before(candidates[right].modTime)
	})
	if len(candidates) == 0 {
		return ""
	}
	return candidates[len(candidates)-1].path
}

func palDefenderLogEventID(path string, offset int64, line string) string {
	digest := sha256.Sum256([]byte(fmt.Sprintf("%s:%d:%s", path, offset, line)))
	return "event_" + hex.EncodeToString(digest[:8])
}
