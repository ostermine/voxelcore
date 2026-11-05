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

## Репликация движения игроков

Движение игроков можно отдать движку: сервер сам читает позиции привязанных игроков и рассылает снимки, клиент сам шлёт
движение своего игрока и плавно расставляет чужих. Скрипты занимаются входом, именами и остальными сообщениями игры.
Сообщения репликации идут по своей полосе (по умолчанию 1) и в `netgame.poll()` не попадают.

```lua
-- Настройка (обе стороны). Все поля необязательны.
netgame.configure_replication({
    lane: int,                 -- полоса сообщений репликации (1)
    delay: number,             -- чужие игроки показываются на столько секунд в прошлом (0.1)
    max_extrapolation: number, -- дальше последнего снимка не больше чем на столько секунд (0.25)
    smoothing: number,         -- скачок от запоздавших снимков (пауза, потери) не телепортом, а скольжением:
                               -- остаток скачка уменьшается вдвое каждые столько секунд (0.08)
    glide_min: number,         -- скачки короче стольких блоков — сразу: их не видно, а скольжение
                               -- держало бы игрока в стороне от пути (0.25)
    snap_distance: number,     -- скачок длиннее стольких блоков — телепорт (8)
    tiers: table,              -- {{дистанция, период}, ...}: ближе дистанции — каждый период-й тик
                               -- (по умолчанию {{50, 1}, {100, 2}, {огромное, 4}})
})

-- Сервер: движение из соединения двигает игрока pid, соединение получает снимки остальных.
netgame.replicate_player(conn: int, pid: int)

-- Клиент: отправлять серверу движение своего игрока pid.
netgame.replicate_local(conn: int, pid: int)

-- Клиент: снимки серверного игрока server_pid двигают своего игрока pid (созданного player.create).
netgame.add_remote(server_pid: int, pid: int)
netgame.remove_remote(server_pid: int)

-- Отвязать соединение (при отключении это делается само).
netgame.unreplicate(conn: int)

-- Каждый тик (после netgame.poll): сервер рассылает снимки, клиент отправляет движение.
netgame.replication_tick()

-- Клиент, каждый кадр (on_hud_render): расставить чужих игроков на текущий момент.
netgame.replication_frame()

-- Счётчики с прошлого вызова: ticks, tick_avg_ms, tick_max_ms, moves, records, messages, bytes, snapshots.
netgame.replication_stats() -> table
```

Чужих игроков движок ставит телепортом каждый кадр и выводит их тела из физики (тип тела `static` до следующего шага):
скорость тела — скорость по снимкам, `is_grounded()` и `is_crouching()` — как у игрока на его клиенте, так что аниматоры
модели видят ходьбу, как у своего игрока. Выделять их прицелом и лучом можно. До первого снимка им нужны
`player.set_flight(pid, true)` и `player.set_noclip(pid, true)` (иначе падают), и всегда — `player.set_loading_chunks(pid, false)`.
Формат сообщений описан в `src/network/netgame/Replication.hpp`.
