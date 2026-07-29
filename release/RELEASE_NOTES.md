## Pal3Mien Launcher v1.0

- Launcher nội bộ v1.0.36 cập nhật PalHud v1.4.9: nhận protocol
  `[PALCOMBAT]` từ PalCombatlog và hiển thị block chiến đấu với thanh cooldown
  đếm ngược; trạng thái combat không còn chiếm khung chat.
- Launcher nội bộ v1.0.35 cập nhật PalHud v1.4.8: không còn gọi `get()` trên
  UObject trực tiếp từ `FindFirstOf`, `FindAllOf` hoặc method return; object
  stale/null bị loại bằng `IsValid()` nên không còn kéo UE4SS vào
  `EXCEPTION_ACCESS_VIOLATION` trong vòng cập nhật HUD.
- Launcher nội bộ v1.0.34 và client PalVerify v1.0.16 dùng bộ đếm heartbeat
  nội bộ theo tiến trình, không còn lấy `sequence` từ đồng hồ máy. Coordinator
  xác thực chống replay bằng challenge dùng một lần nên máy chỉnh lùi thời
  gian hoặc restart client không còn bị kick sai `STALE_REPORT`.
- Launcher nội bộ v1.0.33 cập nhật PalHud v1.4.7: runtime phía client không
  còn tự chạy vòng gửi HUD của server khi đã có local controller, nên gói
  fallback `ĐANG ĐỒNG BỘ` không còn ghi đè ngắt quãng số Gacha thật.
- Launcher nội bộ v1.0.32 đồng bộ PalHud vào cả source Workshop lẫn cache
  UE4SS đang hoạt động tại `Mods\NativeMods\UE4SS\Mods\PalHud`, gồm script,
  logo và trạng thái bật mod, để máy đã cài bản cũ thực sự nạp v1.4.6.
- Launcher nội bộ v1.0.31 và client PalVerify v1.0.15 không còn dùng
  timestamp từ đồng hồ máy người chơi để tính tuổi heartbeat. Coordinator tự
  đóng dấu lúc nhận report, nên máy lệch giờ không còn bị kick sai
  `STALE_REPORT`; client cũ vẫn tương thích trong lúc rollout.
- Launcher nội bộ v1.0.30 cập nhật PalHud v1.4.6: mỗi chu kỳ HUD chỉ giữ
  controller/player state trong đúng một game-thread callback, gửi xong xóa
  ngay toàn bộ UObject để không còn RPC vào controller stale.
- Launcher nội bộ v1.0.29 cập nhật PalHud v1.4.5: production bỏ hoàn toàn
  preview cục bộ, chỉ hiện HUD sau protocol thật trong world và tự collapse
  widget khi local controller `UnPossess` để không bám ra menu chính.
- Launcher nội bộ v1.0.28 cập nhật PalHud v1.4.4: giải phóng cache player
  controller bằng pre-hook `K2_OnLogout` và `UnPossess` trước khi engine hủy
  UObject, tránh RPC HUD gọi lại controller đã stale sau khi người chơi rời.
- Launcher nội bộ v1.0.27 cập nhật PalHud v1.4.3: bỏ hoàn toàn đường chuyển
  `UniqueId` lúc gửi HUD, đối chiếu runtime bằng tên người chơi duy nhất và
  giữ trạng thái đồng bộ nếu tên bị trùng.
- Launcher nội bộ v1.0.26 cập nhật PalHud v1.4.2: protocol HUD chuyển sang
  targeted `ClientMessage` RPC, không còn đi qua
  `PalUtility.SendSystemToPlayerChat` và PalDefender chat transport.
- Launcher nội bộ v1.0.25 cập nhật PalHud v1.4.1: vòng HUD dùng
  `LoopAsync` và chỉ xếp tối đa một callback game-thread, đồng thời dedicated
  server không còn sửa protocol chat outbound hoặc thử dựng UMG.
- Launcher nội bộ v1.0.24 cài và tự kích hoạt PalHud v1.4.0 cho người chơi,
  gồm HUD booster/gacha, phím F5 bật tắt và logo nền. PalBooster vẫn chỉ chạy
  phía server; launcher không phân phối executable hoặc dữ liệu preview local.
- Launcher nội bộ v1.0.23 và client PalVerify v1.0.14 hiển thị hộp thoại
  tiếng Việt nêu rõ lý do, mã lỗi, chi tiết an toàn và cách xử lý trước khi
  server kick. Heartbeat runtime nhận cùng quyết định policy với evaluator;
  client mới vẫn tương thích coordinator cũ trả body rỗng và client cũ vẫn
  bỏ qua body JSON mới. Refresh inventory/hash mỗi phút chạy trên background
  worker để heartbeat 5 giây không bị chặn bởi disk I/O.
- Launcher nội bộ v1.0.22 và client PalVerify v1.0.13 tách cache
  `UE4SSExperimentalPW\Mods\PalVerify` khỏi digest package cha. Cache này vẫn
  được xác minh qua package `PalVerify` riêng, còn UE4SS core vẫn đổi digest
  khi bị sửa. Vì vậy cập nhật client sau này không còn làm UE4SS
  `DIGEST_MISMATCH`.
- Launcher nội bộ v1.0.21 và client PalVerify v1.0.12 chỉ báo cáo bản
  Workshop đang hoạt động trong `Palworld\Mods\Workshop` khi Steam Workshop
  còn giữ một bản nguồn có cùng `PackageName`. Việc này ngăn bản nguồn cũ tạo
  `DIGEST_MISMATCH` giả, trong khi các package có tên khác vẫn được kiểm tra.
- Launcher nội bộ v1.0.20 và client PalVerify v1.0.11 bổ sung bằng chứng
  chẩn đoán an toàn cho DLL/module bị nghi vấn: tên file, mô tả sản phẩm,
  công ty, signer và rule match. Cảnh báo trên máy người chơi và hồ sơ admin
  chỉ hiển thị basename, không gửi đường dẫn đầy đủ hay danh sách process.
  Installer đồng bộ cả watchdog cache đang hoạt động trong
  `Mods\NativeMods\UE4SS`, loại bỏ bản Lua cũ từng gọi `cmd.exe` mỗi 5 giây.
- Launcher nội bộ v1.0.19 và client PalVerify v1.0.10 chuyển quyền sở hữu
  tiến trình về launcher: Lua không còn gọi `cmd.exe` hoặc mở client lặp mỗi
  5 giây. Launcher chỉ tạo một client ẩn, chỉ recovery khi client thật sự mất
  trong lúc Palworld đang chạy; client tự thoát ngay khi game đóng. Chu kỳ
  5 giây chỉ còn dành cho heartbeat xác minh.
- Launcher nội bộ v1.0.18 và client PalVerify v1.0.9 tự chạy agent ngay sau
  preflight, không kill heartbeat khi cập nhật launcher giữa phiên, đồng thời
  cài watchdog vào UE4SS thật sự để khởi động lại agent mỗi 5 giây. Cảnh báo
  DLL giờ ghi basename, SHA-256, signer và rule match an toàn cho admin rà
  false positive mà không gửi đường dẫn đầy đủ.
- Coordinator cho phép cùng mod xuất hiện ở profile local và Steam Workshop
  khi từng bản sao đều khớp package chính xác đã duyệt; một bản có
  version/digest lạ vẫn bị chặn. Policy bổ sung chính xác biến thể UE4SS từ
  Steam Workshop để không kick `DUPLICATE_REPORT` sai sau lần refresh inventory.
- Launcher nội bộ v1.0.17 và client PalVerify v1.0.8 giữ
  `PalVerifyClient.exe` hoạt động suốt phiên bằng watchdog 5 giây. Nếu agent
  bị tắt, client tự chạy lại; nếu heartbeat vẫn mất quá thời hạn, server kick
  `MISSING_PALVERIFY`.
- Coordinator giữ chính xác gói PalVerify liền trước theo version và package
  digest trong lúc rollout, tránh kick `VERSION_MISMATCH` sai đối với phiên đã
  preflight trước khi manifest đổi; payload cùng version nhưng sai digest vẫn
  bị chặn.
- Whitelist chính xác của `StatueMapMarkers` và `UE4SSExperimentalPW` do
  launcher cài được đưa vào source policy dùng chung, tránh preflight cho phép
  nhưng evaluator kick `NOT_WHITELISTED` khi runtime environment khác nhau.
- Server agent bắt buộc kiểm tra cả phiên auto-admin/spectator, sửa đường dẫn
  mặc định tới `PalDefender/Logs`, đồng thời đưa cảnh báo bảo mật PalDefender
  lên admin audit theo nhóm để không làm panel bị flood.
- Launcher nội bộ v1.0.16 tách profile server khỏi các Steam Workshop mod mà
  người chơi đã subscribe, tự cách ly đúng mod bị báo `NOT_WHITELISTED` vào
  `Mods\.pal3mien-quarantine` rồi chạy lại preflight một lần. Dữ liệu mod được
  giữ lại để có thể khôi phục, không sửa trực tiếp đăng ký Workshop của Steam.
- Launcher nội bộ v1.0.15 đặt UE4SS fallback vào đúng Workshop ID
  `3625223587`, để Palworld triển khai gói thành `NativeMods\UE4SS`; cấu trúc
  cũ `Workshop\UE4SSExperimentalPW` do launcher tạo được dọn sau khi sao chép.
- Launcher nội bộ v1.0.14 và client PalVerify v1.0.7 không quét hai lần khi
  `WorkshopRootDir` trỏ đúng vào `Palworld\Mods\Workshop`, tránh lỗi
  `DUPLICATE_REPORT` giả cho PalVerify, UE4SS và StatueMapMarkers.
- Launcher nội bộ v1.0.13 buộc `WorkshopRootDir` trỏ tới runtime UE4SS dự
  phòng khi máy chưa có gói Workshop bên ngoài, để `StatueMapMarkers` được
  Palworld nạp thật sự.
- Launcher nội bộ v1.0.13 chờ người chơi thoát hoàn toàn Palworld trước khi
  cài hoặc cập nhật mod, sau đó tự kiểm tra lại và tiếp tục cài; quá trình cập
  nhật launcher không còn tự tắt game.
- Launcher nội bộ v1.0.12 tự thêm `UE4SSExperimentalPW` vào
  `ActiveModList`, để Palworld thực sự nạp UE4SS trước khi chạy
  `StatueMapMarkers`.
- Launcher nội bộ v1.0.11 tự cài và kích hoạt `StatueMapMarkers`; nếu máy chưa
  có UE4SS Workshop thì launcher dùng runtime dự phòng đi kèm.
- Phát hành chính thức Pal3Mien Launcher và PalVerify v1.0.
- Dùng một đường dẫn tải cố định cho mọi lần cập nhật sau.
- Tự kiểm tra phiên bản, SHA-256 và cài cập nhật bắt buộc.
- Cài PalVerify từ payload nhúng và kiểm tra tính toàn vẹn trước khi chạy.
- Hỗ trợ shortcut Desktop mặc định và log lỗi có thể sao chép gửi admin.
- Sửa lỗi kẹt 55% khi Steam giữ `TargetBuildID` bằng build đã cài hoặc khi
  Palworld đang chạy.
- Sửa lỗi `Ordinal 345` bằng cách kích hoạt Common Controls v6 trong
  `PalVerifyClient.exe`.
- Launcher nội bộ v1.0.7 chỉ bật xanh 100% sau khi client gửi preflight và
  coordinator xác nhận đúng version, hash, mod policy và trạng thái integrity.
- Client PalVerify v1.0.3 quét thêm đúng Workshop root mà Palworld đang dùng,
  nên UE4SS và Workshop mod chưa được whitelist sẽ bị chặn ngay từ preflight.
- Client PalVerify v1.0.3 báo ngay mã lỗi integrity trên máy người chơi thay vì
  chờ server kick mới biết.
- Launcher nội bộ v1.0.8 và client PalVerify v1.0.4 báo riêng mod UE4SS có
  `enabled.txt`, ví dụ `StatueMapMarkers`, thay vì chỉ hiện tên gói UE4SS cha.
- Launcher nội bộ v1.0.9 và client PalVerify v1.0.5 sửa đường dẫn tự khởi động
  runtime agent theo đúng thư mục `Mods\Workshop`, kể cả khi game được restart
  hoặc mở trực tiếp từ Steam.
- Launcher nội bộ v1.0.10 và client PalVerify v1.0.6 giữ runtime agent sống
  thêm hai phút khi Palworld thoát, để client tự nối lại sau một lần restart
  game ngắn thay vì tạo `NO_VALID_REPORT`.
- Launcher và coordinator lấy release manifest qua HTTPS API của website;
  manifest không còn được phục vụ từ GitHub public.
- Launcher tự dừng `PalVerifyClient.exe` đang giữ file trước khi thay payload,
  tránh lỗi `INSTALL_PAYLOAD_FAILED` do Windows sharing violation.
- WinHTTP dùng timeout phù hợp hơn, retry có giới hạn và ghi rõ stage/mã lỗi
  khi coordinator tạm thời không truy cập được.
- Các preflight/report/kick bị từ chối và toàn bộ integrity rule được gửi tới
  Discord audit bằng player name hoặc `player_ref` ẩn danh; không gửi IP,
  UserID đầy đủ, đường dẫn, process/module inventory hoặc webhook secret.
- Integrity violation đã gắn với phiên online sẽ bị ban 24 giờ ở lần đầu và tự
  unban khi hết hạn. Một lần vi phạm mới sau đó sẽ chuyển thành ban vĩnh viễn;
  poll lặp của cùng sự cố không làm tăng số lần vi phạm.
