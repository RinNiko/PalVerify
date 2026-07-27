# StatueMapMarkers

리프멍크 조각상 등 수집형 조각상 407개를 Palworld 월드맵에 표시하고,
가까운 미수집 조각상을 상단 나침반에도 표시하는 UE4SS Lua 모드입니다.

> **Version 1.2.3:** 맵을 처음 열기 전까지 매초 `FindFirstOf("WBP_Map_Base_C")` 하던
> 끊김 제거. Construct/Notify로 세션이 켜진 뒤에만 맵 위젯을 찾고, Notify 부재 시에만
> 폴백 폴링합니다.

> **Version 1.2.2:** 맵 닫힌 채 매초 `FindFirstOf` 폴링하던 끊김 제거. `WBP_Map_Base` 전용
> Construct만 사용하고, 이미 만든 마커가 있으면 visibility probe를 돌리지 않습니다.

> **Version 1.2.1:** 전역 `UserWidget` Construct/Destruct 훅 제거, 맵 세션 토큰으로
> 숨김/교체 시 아이콘 큐 취소, 나침반 GUID 선검사·재사용. 맵을 닫아도 마커는 유지하고
> 다시 열면 재부착합니다.

> **Version 1.1.8:** 풀링된 지도 위젯이 `Construct`를 다시 호출하지 않는 환경에서도,
> 지도가 실제로 열려 있으면 마커 생성 세션을 재개합니다.

> **Version 1.1.0:** 일반 월드와 세계수의 지도 본문을 자동 판별하여
> 각 지도에 해당하는 조각상 마커를 올바르게 표시합니다.

## 동작 방식

- `PalLevelObjectRelic` 객체에서 조각상 종류, 수집 상태와 실제 ObtainFX 좌표를 읽습니다.
- 게임의 `PalUIWorldMapIcon` 위젯을 생성하고 조각상 전용 텍스처를 적용합니다.
- 맵을 열 때 플레이어에게 가까운 조각상부터 여러 틱에 나눠 생성합니다.
- 맵 생성/닫힘 이벤트로 동작하며 맵이 닫혀 있을 때는 게임 스레드 폴링을 중단합니다.
- 마커 생성 중에만 250ms 배치 주기를 사용하고, 생성 완료 후 유지보수는 기본 1초 주기로 낮춥니다.
- 수집한 조각상은 설정된 투명도로 표시합니다.
- `F9` 키로 지도 조각상 마커만 켜거나 끌 수 있으며 나침반 표시는 유지됩니다.
- `F10` 키로 나침반 조각상 마커만 별도로 켜거나 끌 수 있습니다.
- 상단 나침반에는 기본 300m 이내의 가까운 미수집 조각상 최대 5개를 표시합니다.
- 텍스트 마름모와 X 폴백 마커는 사용하지 않습니다.

## 설치

`StatueMapMarkers` 폴더를 다음 위치에 복사합니다.

```text
Pal/Binaries/Win64/ue4ss/Mods/
```

## 설정

`Scripts/config.lua`에서 다음 값을 조절할 수 있습니다.

- `Debug`: 진단 로그만 출력(추가 훅이나 마커 동작 변경 없음)
- `ShowOnMap`: 모드 로드 시 지도 마커 표시 여부
- `MapToggleKey`: 지도 마커만 켜거나 끄는 단축키(기본값 `F9`, `false`로 비활성화)
- `CompassToggleKey`: 나침반 마커만 켜거나 끄는 단축키(기본값 `F10`, `false`로 비활성화)
- `NativeIconSize`: 아이콘 크기(px)
- `UncollectedIconBrightness`: 미수집 아이콘 밝기 배율
- `HideCollectedIcons`: 수집 완료 아이콘을 생성하지 않거나 숨김
- `CollectedIconGrayscale`: 수집 아이콘에 회색 비활성화 효과 적용
- `CollectedIconOpacity`: 수집한 조각상의 투명도
- `ShowInCompass`: 상단 나침반 표시 사용 여부
- `CompassRangeMeters`: 나침반 검색 거리(m, 기본값 `300`)
- `CompassMaxMarkers`: 나침반에 동시에 표시할 최대 개수(기본값 `5`)
- `CompassHideCollected`: 수집 완료 조각상을 나침반에서 숨김
- `CompassUpdateIntervalMs`: 주변 조각상 갱신 주기(ms, 기본값 `5000`)
- `NativeIconBatchSize`: 한 틱에 생성할 아이콘 수
- `NativeIconIntervalMs`: 아이콘 생성 틱 간격
- `ScanIntervalMs`: 일반 갱신 간격
- `StateScanIntervalMs`: 수집 상태 갱신 간격

정상적으로 생성되면 UE4SS 로그에 다음 메시지가 표시됩니다.

```text
Native icons COMPLETE: created=N failed=0
```

나침반 테스트가 정상 동작하면 `Debug = true` 설정 시 다음 로그도 확인할 수 있습니다.

```text
Compass markers: active=N candidates=N range=300m max=5
```
