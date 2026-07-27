# PalVerify server audit và integrity discipline

`PalVerifyServer.exe` gửi các sự kiện enforcement tới Discord khi
`discordWebhookUrl` trong `server-config.json` hoặc biến môi trường
`PALVERIFY_DISCORD_WEBHOOK_URL` được cấu hình.

Không commit webhook thật. Chỉ chấp nhận endpoint HTTPS chính thức của Discord.
Payload audit gồm player name, `player_ref` ẩn danh, server ID, action, reason,
integrity rule, rejected mod, diagnostic detail và trạng thái kỷ luật. Payload
không chứa IP, UserID đầy đủ, đường dẫn client hoặc process/module inventory.

PalDefender REST token của server agent cần các quyền:

- `REST.Players.Read`
- `REST.Punishments.Kick`
- `REST.Punishments.Ban`
- `REST.Punishments.Unban`

Lần integrity violation đầu tiên đã gắn với một phiên online sẽ bị ban 24 giờ.
State được lưu cạnh executable trong `discipline-state.json` theo mặc định để
tự unban sau restart. Sau khi lần ban đầu đã hết hạn, integrity violation tiếp
theo của cùng tài khoản sẽ bị ban vĩnh viễn.

Preflight diễn ra trước khi có phiên online chỉ tạo Discord audit ẩn danh và
khóa launcher. Không tự động ban dựa trên preflight vì identity ở giai đoạn này
chưa được server xác thực.
