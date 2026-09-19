# module_otaclient — OTA-клиент

> **Опциональный модуль.** Подключается через `src_filter` + `build_flags`.
> **Работоспособен только в составе сборки, содержащей ядро** (см. `../../TRS.md` §2.1).

Автоматическое удалённое обновление: опрос манифеста по HTTP, сравнение версий прошивки и
ФС, загрузка и прошивка с повторами и прогрессом. Наследник базового OTA
(`CLASS_MODULE_OTACLIENT : public CLASS_CORE_OTA`).

- **Репозиторий:** https://github.com/Arcanum753/module_otaclient
- **Папка:** `src/module_otaclient/`
- **Флаг активации:** `-D MODULE_OTACLIENT=1`
- **Registry:** только `object=module_otaclient`, `define=MODULE_OTACLIENT`. Размещается в **core**-группах (begin/web_Init/loop) рядом с базовым OTA, а не через флаги `web`/`loop` (особый случай генератора registry).
- **Только для платформы:** обе (ESP8266/ESP32)
- **Зависит от модулей:** —
- **Зависит от ядра:** `core_ota` (базовый класс и общие маршруты `/update*`), `core_web`, `core_sys`, `core_json`, `core_wifi`, `core_ntp`

## Назначение

Автоматическое удалённое обновление — опрос манифеста по HTTP, сравнение версий FW/FS,
загрузка и прошивка с повторами и прогрессом.

> [!WARNING]
> **Данный модуль находится в стадии активной разработки. API и функционал могут изменяться. Подробное руководство пользователя будет добавлено по мере стабилизации.**

## Функциональные требования

- FR-OTACLIENT-1: Периодический опрос манифеста (`checkForUpdates`) по `serverAddress:serverPort/manifestPath` с таймаутом `timeOut`.
- FR-OTACLIENT-2: Сравнение версий FW/FS, загрузка и прошивка при необходимости; `OTACLIENT_MAX_RETRIES=3`, `OTACLIENT_CHUNK_SIZE=1024/512`, до `OTACLIENT_MAX_MANIFEST_ENTRIES=20` записей.
- FR-OTACLIENT-3: Запуск проверки по подключению Wi-Fi (`onWiFiConnect`) и/или по включению питания (`powerOn`).
- FR-OTACLIENT-4: Конфиг `/config_otaclient.json`: `timeOut`, `powerOn`, `serverAddress`, `serverPort`, `manifestPath`.
- FR-OTACLIENT-5: Тестовое подключение (`/otaclient/test`, `/otaclient/teststatus`).
- FR-OTACLIENT-6: Наследует общие маршруты OTA (`/update*`) и ресурсы `ota.state`, `ota.server_reachable`.

## Веб-интерфейс

Маршруты: `POST /otaclient.html`, `GET /otaclient/info`, `/otaclient/test`,
`/otaclient/teststatus`, `/otaclient/update`, `/otaclient/ver`.
Страница `otaclient.html` (пункт меню — `web/_menu.html`); наследует общие маршруты OTA.

## Конфигурация

`/config_otaclient.json` — настройки OTA-клиента (адрес сервера, порт, путь к манифесту,
таймаут, запуск по питанию). Поля: см. FR-OTACLIENT-4.

## Слоистая структура

Из `../../LAYERS.md`: `module_otaclient` — OTA-клиент: manifest, мини-HTTP; сейчас define'ы,
`strOtaClientConfig`, `ManifestEntry`; выделить `_types.h`, `_engine.cpp`.

## Ссылки

- Ядро и конвенции: `../../TRS.md`
- Слоистая структура: `../../LAYERS.md`
- Общие утилиты: `../../TRS.md` §3.1.12 (`common/`)
- Сборка: `../../BUILD.md`
- Реестр компонентов: `../../INVENTORY.md`

> Если модуль читается вне дерева ядра (standalone), корневые документы доступны в
> репозитории ядра avr-fota.
