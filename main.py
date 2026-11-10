import os
import hmac
import hashlib
import base64
import json
import uuid
from collections import deque
from threading import Lock

import httpx
from fastapi import FastAPI, Request, Header, HTTPException
from fastapi.responses import JSONResponse

app = FastAPI(title="ESP32 LINE Cloud Bridge")

LINE_CHANNEL_SECRET = os.getenv("LINE_CHANNEL_SECRET", "")
LINE_CHANNEL_ACCESS_TOKEN = os.getenv("LINE_CHANNEL_ACCESS_TOKEN", "")
LINE_USER_ID = os.getenv("LINE_USER_ID", "")
DEVICE_KEY = os.getenv("DEVICE_KEY", "")

LINE_REPLY_URL = "https://api.line.me/v2/bot/message/reply"
LINE_PUSH_URL = "https://api.line.me/v2/bot/message/push"

# One ESP32 device: one active command at a time.
pending_commands = deque()
active_command = None
state_lock = Lock()
seen_webhook_events = set()
default_user_id = LINE_USER_ID


def verify_line_signature(body: bytes, signature: str | None) -> bool:
    if not LINE_CHANNEL_SECRET or not signature:
        return False
    digest = hmac.new(
        LINE_CHANNEL_SECRET.encode("utf-8"),
        body,
        hashlib.sha256
    ).digest()
    expected = base64.b64encode(digest).decode("ascii")
    return hmac.compare_digest(expected, signature)


def check_device_key(value: str | None):
    if not DEVICE_KEY or not value or not hmac.compare_digest(value, DEVICE_KEY):
        raise HTTPException(status_code=401, detail="Unauthorized device")


async def line_reply(reply_token: str, message: str):
    if not LINE_CHANNEL_ACCESS_TOKEN:
        return
    headers = {
        "Authorization": f"Bearer {LINE_CHANNEL_ACCESS_TOKEN}",
        "Content-Type": "application/json",
    }
    payload = {
        "replyToken": reply_token,
        "messages": [{"type": "text", "text": message[:4900]}],
    }
    async with httpx.AsyncClient(timeout=15) as client:
        response = await client.post(LINE_REPLY_URL, headers=headers, json=payload)
        if response.status_code >= 400:
            print("LINE reply error:", response.status_code, response.text)


async def line_push(user_id: str, message: str):
    if not LINE_CHANNEL_ACCESS_TOKEN:
        raise RuntimeError("LINE_CHANNEL_ACCESS_TOKEN is not configured")
    if not user_id:
        raise RuntimeError("No LINE user ID is available")
    headers = {
        "Authorization": f"Bearer {LINE_CHANNEL_ACCESS_TOKEN}",
        "Content-Type": "application/json",
    }
    payload = {
        "to": user_id,
        "messages": [{"type": "text", "text": message[:4900]}],
    }
    async with httpx.AsyncClient(timeout=15) as client:
        response = await client.post(LINE_PUSH_URL, headers=headers, json=payload)
        if response.status_code >= 400:
            print("LINE push error:", response.status_code, response.text)
            raise HTTPException(status_code=502, detail="LINE push failed")


@app.get("/")
async def health():
    return {"status": "ok", "service": "ESP32 LINE Cloud Bridge"}


@app.post("/line/webhook")
async def line_webhook(request: Request):
    global default_user_id
    raw_body = await request.body()
    signature = request.headers.get("x-line-signature")

    # Verify the untouched raw request body before JSON parsing.
    if not verify_line_signature(raw_body, signature):
        raise HTTPException(status_code=401, detail="Invalid LINE signature")

    try:
        body = json.loads(raw_body.decode("utf-8"))
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")

    for event in body.get("events", []):
        if event.get("type") != "message":
            continue

        message = event.get("message", {})
        if message.get("type") != "text":
            continue

        text = message.get("text", "").strip()
        reply_token = event.get("replyToken", "")
        source = event.get("source", {})
        user_id = source.get("userId", "")
        if user_id:
            default_user_id = user_id

        # Accept LINE commands: "เปิด" and "เช็คความชื้น".
        if text in ("เปิด", "เช็คความชื้น"):
            event_id = event.get("webhookEventId") or str(uuid.uuid4())
            command = "OPEN" if text == "เปิด" else "CHECK_HUMIDITY"

            is_new_event = False
            with state_lock:
                if event_id not in seen_webhook_events:
                    seen_webhook_events.add(event_id)
                    pending_commands.append({
                        "command_id": str(uuid.uuid4()),
                        "command": command,
                        "user_id": user_id,
                    })
                    is_new_event = True

                    # Keep the set bounded for a long-running service.
                    if len(seen_webhook_events) > 2000:
                        seen_webhook_events.clear()

            if reply_token and is_new_event:
                if command == "OPEN":
                    acknowledgement = "ได้รับคำสั่งเปิดแล้ว กำลังตรวจสอบความชื้นดิน..."
                else:
                    acknowledgement = "ได้รับคำสั่งแล้ว กำลังอ่านค่าความชื้นจาก ESP32..."
                await line_reply(reply_token, acknowledgement)


    return {"status": "ok"}


@app.get("/device/command")
async def get_device_command(x_device_key: str | None = Header(default=None)):
    check_device_key(x_device_key)

    global active_command
    with state_lock:
        if active_command is None and pending_commands:
            active_command = pending_commands.popleft()

        if active_command is None:
            return JSONResponse({"command": None})

        # Keep returning the same command until ESP32 submits its result.
        return JSONResponse({
            "command_id": active_command["command_id"],
            "command": active_command["command"],
        })


@app.post("/device/result")
async def device_result(request: Request, x_device_key: str | None = Header(default=None)):
    global active_command
    check_device_key(x_device_key)

    try:
        data = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")

    command_id = str(data.get("command_id", ""))
    with state_lock:
        current = active_command
        if current is None or current["command_id"] != command_id:
            raise HTTPException(status_code=409, detail="Unknown or expired command")
        target_user = current.get("user_id") or LINE_USER_ID

    humidity = int(data.get("humidity", 0))
    raw = int(data.get("raw", 0))
    relay1 = str(data.get("relay1", "OFF"))
    relay2 = str(data.get("relay2", "OFF"))
    mode = str(data.get("mode", "AUTO"))

    # For the LINE command "เปิด", use the decision/reply from ESP32
    # after it has freshly read the soil sensor.
    if current.get("command") == "OPEN":
        message = str(data.get("reply", "")).strip()
        if not message:
            # Fallback for older ESP32 firmware that does not send "reply".
            if humidity > 50:
                message = "สถานะดินตอนนี้ชื้นเพียงพอแล้ว"
            else:
                message = (
                    f"ความชื้นดิน {humidity}% ระบบเริ่มทำงานแล้ว\\n"
                    f"วาล์ว 1: {relay1}\\n"
                    f"วาล์ว 2: {relay2}"
                )
    else:
        if humidity <= 40:
            soil_status = "ดินแห้ง"
        elif humidity < 60:
            soil_status = "ความชื้นปานกลาง"
        else:
            soil_status = "ดินชุ่มชื้น"

        message = (
            "ผลการตรวจสอบความชื้นดิน\\n"
            f"ความชื้นดิน: {humidity}%\\n"
            f"สถานะ: {soil_status}\\n"
            f"ค่าเซนเซอร์ดิบ: {raw}\\n"
            f"วาล์ว 1: {relay1}\\n"
            f"วาล์ว 2: {relay2}\\n"
            f"โหมดการทำงาน: {mode}"
        )

    await line_push(target_user, message)

    with state_lock:
        if active_command and active_command["command_id"] == command_id:
            active_command = None

    return {"status": "sent"}


@app.post("/device/report")
async def device_report(request: Request, x_device_key: str | None = Header(default=None)):
    check_device_key(x_device_key)

    try:
        data = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")

    message = str(data.get("message", "")).strip()
    if not message:
        raise HTTPException(status_code=400, detail="Message is empty")
    target_user = LINE_USER_ID or default_user_id
    if not target_user:
        raise HTTPException(status_code=500, detail="No LINE recipient yet; send a message to the Official Account first")

    await line_push(target_user, message)
    return {"status": "sent"}
