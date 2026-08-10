#pragma once

// fix33 — REST/UI-обёртка над system_time_config singleton row.
//
// load(): читает row из AuthDb. Если row отсутствует (свежая БД до
//         первого save), возвращает std::nullopt — REST-слой превращает
//         в "configured":false и отдаёт defaults наружу. В отличие от
//         LdapConfigRepo здесь нет шифрования: поля без секретов.
// save(): UPSERT'ит row, выставляет updated_at = now.
// updateNtpSyncResult(): частичный апдейт двух полей — поллер дёргает
//         его после каждого успешного SNTP-обмена, не трогая остальной
//         конфиг (избегаем гонок с админ-правкой через UI).
// validate(): возвращает пустую строку при OK, иначе человекочитаемое
//         сообщение об ошибке (для отдачи в REST detail). Проверяет:
//         IANA-тайм-зону, server-list при source=Ntp, диапазоны.

#include <cstdint>
#include <optional>
#include <string>

#include "auth/AuthDb.h"
#include "auth/TimeConfig.h"

namespace liveqx::auth {

class TimeConfigRepo {
public:
    explicit TimeConfigRepo(AuthDb& db);

    std::optional<TimeConfig> load();
    bool save(const TimeConfig& cfg);

    // Атомарный апдейт только NTP-sync результата (для фонового поллера).
    bool updateNtpSyncResult(std::int64_t offset_ms,
                             std::int64_t sync_at_unix_sec);

    // Статический валидатор. Пустая строка = валиден.
    static std::string validate(const TimeConfig& cfg);

private:
    AuthDb& db_;
};

}  // namespace liveqx::auth
