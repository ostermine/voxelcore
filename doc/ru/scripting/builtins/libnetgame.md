# Библиотека *netgame*

Игровой транспорт поверх [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets):
соединения с сообщениями (не поток байт), надёжная и ненадёжная доставка, полосы (lanes) с приоритетами,
шифрование канала, эмуляция задержки и потерь.

Библиотека есть только в сборках с опцией CMake `VOXELENGINE_NETGAME` (на Windows она же включает
feature `netgame` манифеста vcpkg). В остальных сборках `netgame == nil`. Как и *network*, требует
разрешения `network` в проекте.

Функции вызываются из основного потока. События копятся, пока не вызван `netgame.poll()`, поэтому
опрашивать нужно регулярно, например в `on_world_tick`.

## Соединения

```lua
-- Начинает принимать соединения на порту (все интерфейсы).
-- Входящие соединения принимаются автоматически.
netgame.listen(port: int) -> int

-- Перестаёт принимать новые соединения.
netgame.close_listen(listener: int)

-- Подключается к IP-адресу (IPv4, IPv6 или "localhost"). Имена хостов не поддерживаются.
-- Об установке соединения сообщит событие connected.
netgame.connect(address: string, port: int) -> int

-- Закрывает соединение. Уже отправленные надёжные сообщения будут доставлены.
-- Собеседник получит событие disconnected с текстом reason.
netgame.close(conn: int, [опционально] reason: string)

-- Закрывает все соединения и освобождает транспорт.
netgame.shutdown()
```

## Сообщения

```lua
-- Ставит сообщение в очередь отправки. Возвращает false, если соединение
-- закрыто или полоса не настроена.
netgame.send(
    conn: int,
    data: string | Bytearray,
    [опционально] options: {
        -- Доставить с повторами и по порядку (по умолчанию true)
        reliable: boolean,
        -- Подождать до 5 мс, чтобы склеить с другими сообщениями (по умолчанию true)
        nagle: boolean,
        -- Номер полосы (по умолчанию 0)
        lane: int,
    }
) -> boolean

-- Отправляет накопленное по всем соединениям, не дожидаясь Nagle.
-- Удобно вызывать в конце тика.
netgame.flush()

-- Настраивает полосы соединения: priorities[i] — приоритет полосы i - 1
-- (меньше — раньше), weights — доли канала между полосами одного приоритета.
netgame.configure_lanes(conn: int, priorities: table<int>, [опционально] weights: table<int>)
```

Сообщение — до 512 КБ, большие режутся и собираются транспортом.
Без `flush()` и без `nagle = false` надёжное сообщение может ждать отправки до 5 мс;
в обмене «запрос — ответ» задержки складываются.

## События

```lua
-- Выполняет работу транспорта и возвращает события с прошлого вызова.
netgame.poll() -> table<event>
```

Каждое событие — таблица с полем `conn` и полем `type`:

| `type` | Поля | Когда |
|---|---|---|
| `"connected"` | `listener`, `address` | Соединение установлено. `listener` — id из `listen` для входящих, 0 для исходящих |
| `"message"` | `lane`, `data` (Bytearray) | Пришло сообщение |
| `"disconnected"` | `listener`, `reason`, `message` | Собеседник закрыл соединение или оно оборвалось по таймауту. `message` — текст причины |

## Диагностика

```lua
-- Состояние соединения или nil, если соединения нет.
netgame.get_stats(conn: int) -> {
    ping: int,                 -- мс
    quality_local: number,     -- доля дошедших пакетов, 0..1
    quality_remote: number,
    out_bytes_per_sec: number,
    in_bytes_per_sec: number,
    send_rate: int,            -- оценка пропускной способности, байт/с
    pending_reliable: int,     -- байт в очереди
    pending_unreliable: int,
    queue_time_us: int,        -- сколько ждало бы новое сообщение
}

-- Эмулирует задержку (мс) и потерю пакетов (%) при отправке. Действует на весь процесс.
netgame.set_fake_network(lag_ms: int, loss_percent: number)

-- Ограничения скорости отправки, байт/с (по умолчанию максимум 256 КБ/с на соединение).
netgame.set_send_rate(min: int, max: int)
```

## Пример

```lua
local listener = netgame.listen(27020)

function on_world_tick()
    for _, event in ipairs(netgame.poll()) do
        if event.type == "connected" then
            print("connected", event.conn, event.address)
        elseif event.type == "message" then
            netgame.send(event.conn, event.data) -- эхо
        elseif event.type == "disconnected" then
            print("disconnected", event.conn, event.message)
        end
    end
    netgame.flush()
end
```

Соединения без сертификатов разрешены: канал шифруется, но личность собеседника не проверяется.
Аутентификацию игроков нужно делать на уровне игры.
