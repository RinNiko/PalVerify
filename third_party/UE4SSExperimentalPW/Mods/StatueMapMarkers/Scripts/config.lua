return {
    -- Print diagnostic messages to UE4SS.log. This does not install extra
    -- hooks or change marker behavior.
    Debug = false,

    -- Map and compass markers can be enabled and toggled independently.
    -- Use a UE4SS Key table name such as "F9", "M", or "NUM_ZERO".
    -- Set a key to false to disable only that shortcut.
    ShowOnMap = true,
    MapToggleKey = "F9",
    CompassToggleKey = "F10",

    -- Native statue icon appearance.
    NativeIconSize = 30,
    UncollectedIconBrightness = 1.5,
    -- true: skip icons that are already collected and hide newly collected
    -- icons on the next state refresh.
    HideCollectedIcons = true,
    CollectedIconGrayscale = true,
    CollectedIconOpacity = 0.45,

    -- Also show nearby statues on the top compass when the mod loads.
    -- Only a small nearest set is registered to avoid filling the compass
    -- with all 407 world markers. Distances are measured in metres.
    ShowInCompass = true,
    CompassRangeMeters = 300,
    -- Spatial hash cell size used by the compass candidate query. With the
    -- default 300m range this searches only the surrounding 3x3 cells.
    CompassGridCellMeters = 300,
    CompassMaxMarkers = 5,
    CompassHideCollected = true,
    -- Re-evaluate nearby statues at this cadence. Existing markers remain
    -- registered until they leave the range, avoiding periodic widget churn.
    -- Compass stays enabled by default; refresh work is kept cheap instead of
    -- lengthening this interval.
    CompassUpdateIntervalMs = 5000,

    -- Create the nearest markers first, split across small batches.
    -- All markers still appear; small batches avoid a large map-open hitch.
    NativeIconBatchSize = 10,
    NativeIconIntervalMs = 250,

    -- General map update and collected-state refresh intervals (milliseconds).
    ScanIntervalMs = 1000,
    StateScanIntervalMs = 60000,
    StateScanBatchSize = 8,

    -- Pickup notifications can arrive before bPickedInClient replication.
    -- Recheck only the notified sibling set; never wake the 407-actor poll.
    PickupRecheckDelayMs = 1000,
    PickupRecheckMaxAttempts = 3,

    -- Before the first map build, wait for collection replication to settle
    -- and verify flags in batches. Only uncollected widgets are then created;
    -- compass behavior is unchanged.
    MapInitialStateDelayMs = 3000,
    MapInitialStateQuietMs = 1500,
    MapInitialStateBatchSize = 64,

    -- Diagnostics: 0 = disabled (default). Set e.g. 60000 to write a "DIAG"
    -- counter line to UE4SS.log every 60 seconds when investigating issues.
    DiagLogIntervalMs = 0,
}
