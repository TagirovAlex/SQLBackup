# SQLBackup

Утилита для автоматической обработки и доставки MSSQL-архивов резервных копий. Находит свежий файл бэкапа, переименовывает по шаблону, копирует на удалённый сервер, проверяет целостность и отправляет HTML-отчёт по email.

## Возможности

- Поиск самого свежего файла бэкапа в заданной директории
- Переименование по настраиваемому шаблону с подстановкой текущей даты
- Копирование по сетевому пути (UNC / локальный) через `CopyFileEx`
- Верификация копирования по SHA-256
- Автоматическое удаление исходного файла после успешного копирования
- Отправка HTML-письма через Exchange SMTP с таблицей и кликабельными ссылками
- Поддержка SMTP-аутентификации (AUTH LOGIN)
- Подробное логирование: отдельный файл при каждом запуске, автоочистка старых логов
- Настройка через INI-файл

## Требования

- ОС: Windows 7 / Windows Server 2008 R2 и выше
- Компилятор: MSVC 2019+ (Visual Studio 16 2019 / 17 2022)
- Система сборки: CMake 3.15+
- Зависимости: только Windows SDK (Winsock2, CryptoAPI)

## Сборка

```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Исполняемый файл: `build\Release\SQLBackup.exe`

## Конфигурация

Файл `config.ini` размещается рядом с `SQLBackup.exe`. Альтернативный путь — через аргумент `-c`.

```ini
[General]
NameTemplate = backup_{date}
DateFormat = %d.%m.%Y
FileExtension = .bak
Debug = false

; Несколько папок — каждая своей секцией [Backup:<имя>]
[Backup:DB1]
SourcePath = C:\Backups\DB1
DestPath = \\server1\share\DB1

[Backup:DB2]
SourcePath = C:\Backups\DB2
DestPath = \\server2\share\DB2
NameTemplate = archive_{date}

[Mail]
Server = exchange.company.local
Port = 25
SenderName = SQL Backup Service
SenderEmail = backup@company.local
Recipients = admin@company.local, dba@company.local
Auth = false
Username =
Password =

[Log]
LogPath = C:\Logs\SQLBackup
MaxLogs = 30
```

### Параметры

| Параметр | Описание |
|---|---|
| `SourcePath` | Путь к папке с файлами бэкапов (в `[Backup:*]` или `[General]`) |
| `DestPath` | Путь назначения, локальный или UNC (в `[Backup:*]` или `[General]`) |
| `NameTemplate` | Шаблон имени, `{date}` заменяется на дату |
| `DateFormat` | Формат даты в стиле `strftime` |
| `FileExtension` | Расширение искомых файлов |
| `Debug` | Отладочный режим — исходный файл НЕ удаляется |
| `OnExists` | Поведение при существующем файле назначения: 1 — перезаписать, 2 — ошибка, 3 — суффикс _001/_002..., 4 — пропустить |
| `Server` | Адрес Exchange / SMTP-сервера |
| `Port` | SMTP-порт |
| `SenderName` | Отображаемое имя отправителя |
| `SenderEmail` | Email отправителя |
| `Recipients` | Список получателей через запятую |
| `Auth` | Включить SMTP-аутентификацию |
| `Username` | Логин для SMTP |
| `Password` | Пароль для SMTP |
| `TemplatePath` | Путь к HTML-шаблону письма (по умолчанию `mail_template.html`) |
| `LogPath` | Папка для логов |
| `MaxLogs` | Максимальное количество хранимых логов |

## Использование

```cmd
SQLBackup.exe
SQLBackup.exe -c C:\path\to\config.ini
SQLBackup.exe --help
```

### Аргументы командной строки

| Аргумент | Описание |
|---|---|
| `-c <path>` / `--config <path>` | Путь к файлу конфигурации |
| `--help` | Показать справку |

## Шаблон письма

Письмо формируется из HTML-файла, путь к которому задаётся параметром `TemplatePath` в секции `[Mail]` (по умолчанию — `mail_template.html` рядом с exe). В шаблоне поддерживаются переменные:

| Переменная | Описание |
|---|---|
| `{LABEL}` | Метка секции (например `DB1`, `General`) |
| `{FILENAME}` | Имя файла |
| `{FILESIZE}` | Размер файла (в ГБ/МБ/КБ) |
| `{SOURCEPATH}` | Полный путь к исходному файлу |
| `{DESTPATH}` | Полный путь к файлу назначения |
| `{SOURCEURL}` | `file:///` URL исходного файла |
| `{DESTURL}` | `file:///` URL файла назначения |
| `{COPYDATE}` | Дата и время копирования |

## Логирование

При каждом запуске создаётся файл `SQLBackup_YYYYMMDD_HHMMSS.log` в папке, указанной в `LogPath`. При превышении `MaxLogs` самые старые логи автоматически удаляются.

### Множественные папки бэкапов

Для каждой базы данных создаётся отдельная секция `[Backup:<имя>]`:

```ini
[Backup:DB1]
SourcePath = C:\Backups\DB1
DestPath = \\server1\share\DB1

[Backup:DB2]
SourcePath = C:\Backups\DB2
DestPath = \\server2\share\DB2
```

Параметры `NameTemplate`, `DateFormat`, `FileExtension` наследуются из `[General]`, если не указаны в секции. При отсутствии секций `[Backup:*]` используются `SourcePath` и `DestPath` из `[General]` (обратная совместимость).

## Алгоритм работы

1. Загрузка конфигурации из INI-файла
2. Инициализация логгера, очистка старых логов
3. Поиск самого свежего файла с заданным расширением в `SourcePath`
4. Генерация нового имени по шаблону с текущей датой
5. Переименование файла
6. Копирование в `DestPath`
7. Верификация скопированного файла по SHA-256
8. Удаление исходного файла
9. Отправка HTML-письма получателям (если настроено)
10. Завершение

## Требования к правам

Утилита запускается от имени пользователя, имеющего:
- Права на чтение исходной папки и удаление файлов
- Права на запись в сетевую папку назначения
- Доступ к SMTP-серверу (если используется аутентификация — соответствующий логин/пароль)

## Лицензия

MIT
