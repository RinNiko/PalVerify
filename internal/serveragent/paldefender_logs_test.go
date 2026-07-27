package serveragent

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestParsePalDefenderLogLineBuildsSanitizedSecurityEvents(t *testing.T) {
	t.Parallel()
	now := time.Date(2026, 7, 26, 3, 24, 39, 0, time.UTC)
	identity := make(map[string]palDefenderIdentity)

	login := "[20:24:11][info] 'RayJacobs' (UserId=steam_76561198317031083, IP=183.80.101.162) has logged in."
	if event := parsePalDefenderLogLine(
		login,
		identity,
		"bnb",
		"server-secret",
		now,
		"event_0000000000000001",
	); event != nil {
		t.Fatal("login identity line must not become an audit event")
	}

	ban := "[20:24:39][info] Banned UserId='steam_76561198317031083' UserIP='' by rest::'PalVerify' from '127.0.0.1' reason='PalVerify: 24-hour integrity suspension (PROCESS_CHEAT_ENGINE_RUNNING)'"
	event := parsePalDefenderLogLine(
		ban,
		identity,
		"bnb",
		"server-secret",
		now,
		"event_0000000000000002",
	)
	if event == nil {
		t.Fatal("ban line was not parsed")
	}
	if event.Source != "paldefender" ||
		event.Action != "PALDEFENDER_BAN" ||
		event.PlayerName != "RayJacobs" ||
		event.UserID != "steam_76561198317031083" ||
		event.IP != "183.80.101.162" {
		t.Fatalf("unexpected parsed event: %#v", event)
	}
	if event.Detail == ban {
		t.Fatal("raw PalDefender log must not be forwarded")
	}
}

func TestCollectPalDefenderAuditsTailsEachLogLineOnce(t *testing.T) {
	t.Parallel()
	directory := t.TempDir()
	logPath := filepath.Join(directory, "26.07 03.24.00.log")
	body := []byte(
		"[03:24:11][info] 'RayJacobs' (UserId=steam_76561198317031083, IP=183.80.101.162) has logged in.\n" +
			"[03:24:39][info] Banned UserId='steam_76561198317031083' UserIP='' by rest::'PalVerify' from '127.0.0.1' reason='Manual review'\n",
	)
	if err := os.WriteFile(logPath, body, 0o600); err != nil {
		t.Fatal(err)
	}
	state := disciplineState{
		Version: 1,
		Players: make(map[string]disciplineRecord),
	}
	config := Config{
		ServerID:                "bnb",
		ServerToken:             "server-secret",
		PalDefenderLogDirectory: directory,
	}
	collected, err := collectPalDefenderAudits(
		config,
		&state,
		time.Now(),
	)
	if err != nil {
		t.Fatal(err)
	}
	if collected != 1 || len(state.PendingAudits) != 1 {
		t.Fatalf("unexpected collected events: %d %#v", collected, state)
	}
	again, err := collectPalDefenderAudits(config, &state, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	if again != 0 || len(state.PendingAudits) != 1 {
		t.Fatal("collector replayed an already consumed log line")
	}
}

func TestParsePalDefenderCheatLogPreservesReasonWithoutRawLine(t *testing.T) {
	t.Parallel()
	line := "[09:11:51][warn] 'RayJacobs' (UserId=steam_76561198317031083, IP=183.80.101.162) *may be* a cheater! Reason: Attempted to pickup object from a significant distance (9999). Not taking any actions, since this requires human judgement."
	event := parsePalDefenderLogLine(
		line,
		map[string]palDefenderIdentity{},
		"bnb",
		"server-secret",
		time.Now(),
		"event_0000000000000003",
	)
	if event == nil || event.Action != "PALDEFENDER_ALERT" {
		t.Fatalf("unexpected event: %#v", event)
	}
	if event.Reason != "REMOTE_PICKUP" ||
		event.Detail != "Attempted to pickup object from a significant distance (9999)." {
		t.Fatalf("unexpected safe detail: %#v", event)
	}
}

func TestParsePalDefenderBuildingDamageThresholdAlert(t *testing.T) {
	t.Parallel()
	line := "[15:58:30][info] RequestDamageMapObject_ToServer: '7749/' (UserId=steam_00000000000000001, IP=203.0.113.10) have tried to deal more damage (390) than pvpMaxBuildingDamage threshold (100)"
	event := parsePalDefenderLogLine(
		line,
		map[string]palDefenderIdentity{},
		"bnb",
		"server-secret",
		time.Now(),
		"event_0000000000000004",
	)
	if event == nil {
		t.Fatal("building damage threshold line was not parsed")
	}
	if event.Action != "PALDEFENDER_ALERT" ||
		event.Reason != "DAMAGE_THRESHOLD_EXCEEDED" ||
		event.Detail != "Attempted building damage 390 exceeded threshold 100." {
		t.Fatalf("unexpected building damage event: %#v", event)
	}
}

func TestCollectPalDefenderAuditsAggregatesRepeatedSecurityWarnings(t *testing.T) {
	t.Parallel()
	directory := t.TempDir()
	cheatsDirectory := filepath.Join(directory, "Cheats")
	if err := os.MkdirAll(cheatsDirectory, 0o700); err != nil {
		t.Fatal(err)
	}
	line := "[09:11:51][warn] 'RayJacobs' (UserId=steam_00000000000000001, IP=203.0.113.10) *may be* a cheater! Reason: Attempted to pickup object from a significant distance (9999). Not taking any actions, since this requires human judgement."
	body := strings.Repeat(line+"\n", 60)
	if err := os.WriteFile(
		filepath.Join(cheatsDirectory, "27.07 09.11.00.log"),
		[]byte(body),
		0o600,
	); err != nil {
		t.Fatal(err)
	}
	state := disciplineState{
		Version: 1,
		Players: make(map[string]disciplineRecord),
	}
	config := Config{
		ServerID:                "bnb",
		ServerToken:             "server-secret",
		PalDefenderLogDirectory: directory,
	}

	collected, err := collectPalDefenderAudits(config, &state, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	if collected != 1 || len(state.PendingAudits) != 1 {
		t.Fatalf(
			"repeated warnings must become one aggregated event: collected=%d pending=%d",
			collected,
			len(state.PendingAudits),
		)
	}
	if !strings.Contains(state.PendingAudits[0].Detail, "occurrences=60") {
		t.Fatalf("aggregate count is missing: %#v", state.PendingAudits[0])
	}
}

func TestPalDefenderCheatReasonClassifiesKnownSecurityWarnings(t *testing.T) {
	t.Parallel()
	cases := map[string]string{
		"Attempted to RequestUnlockFastTravelPoint_ToServer from too far away: 493678.06m.": "FAST_TRAVEL_DISTANCE",
		"Attempted to pickup DroppedCharacter from too far away (120.0m).":                  "REMOTE_CHARACTER_PICKUP",
		"Attempted to pickup object from a significant distance (9999).":                    "REMOTE_PICKUP",
		"RequestMoveItemToInventoryFromContainer CanInteractWithContainer check failure.":   "CONTAINER_ACCESS_REJECTED",
		"Attempted to select a Pal Sphere that they don't have in inventory: PalSphere.":    "INVALID_SPHERE_SELECTION",
	}
	for detail, expected := range cases {
		if actual := palDefenderCheatReason(detail); actual != expected {
			t.Errorf("unexpected classification for %q: got %q want %q", detail, actual, expected)
		}
	}
}

func TestUpdateConnectedPlayerTracksLoginAutoAdminAndLogout(t *testing.T) {
	t.Parallel()
	connected := make(map[string]connectedPlayer)
	login := "[09:50:38][info] 'RayJacobs' (UserId=steam_00000000000000001, IP=203.0.113.10) has logged in with auto admin mode."
	if !updateConnectedPlayer(login, connected) ||
		connected["steam_00000000000000001"].Name != "RayJacobs" {
		t.Fatalf("auto-admin login was not tracked: %#v", connected)
	}
	logout := "[09:58:28][info] 'RayJacobs' (UserId=steam_00000000000000001, IP=203.0.113.10) has logged out."
	if !updateConnectedPlayer(logout, connected) ||
		len(connected) != 0 {
		t.Fatalf("logout did not clear connected player: %#v", connected)
	}
}

func TestParsePalDefenderPlayerSecurityWarnings(t *testing.T) {
	t.Parallel()
	cases := map[string]string{
		"[10:00:00][warning] 'Player' (UserId=steam_00000000000000001, IP=203.0.113.10) CanInteractWithGroupID failure. Player guild: private != Container guild: private.": "GROUP_ACCESS_REJECTED",
		"[10:00:01][warning] Illegal access attempt from ''Player' (UserId=steam_00000000000000001, IP=203.0.113.10)'. Player guild: private; Target: private":              "ILLEGAL_GROUP_ACCESS",
		"[10:00:02][warning] 'Player' (UserId=steam_00000000000000001, IP=203.0.113.10) attempted to deal damage without Attacker?":                                         "MISSING_DAMAGE_ATTACKER",
	}
	for line, expected := range cases {
		event := parsePalDefenderLogLine(
			line,
			map[string]palDefenderIdentity{},
			"bnb",
			"server-secret",
			time.Now(),
			"event_0000000000000005",
		)
		if event == nil || event.Reason != expected {
			t.Errorf("unexpected security warning event for %q: %#v", expected, event)
		}
		if strings.Contains(event.Detail, "private") {
			t.Errorf("raw guild identifiers leaked into detail: %#v", event)
		}
	}
}
