/*
 * Разбор форм настроек — чистая логика без обращений к железу.
 *
 * Ядро работает на std::string, чтобы собираться на хосте и покрываться
 * тестами. Для прошивки есть тонкая обёртка над Arduino String.
 */

#pragma once

#include <string>

// Маркер явной очистки поля.
//
// Пустое значение означает «не менять» — иначе форма, отправленная
// с незаполненным полем, стирает сохранённую настройку. Именно так
// правка hostname стирала SSID и пароль, после чего устройство
// уходило в AP-режим.
//
// Но полностью запретить очистку нельзя: mqttEnabled выводится из длины
// адреса брокера, то есть без сброса поля MQTT стало бы невозможно
// выключить. Отсюда отдельный маркер.
#define SETTINGS_CLEAR_TOKEN "__CLEAR__"

// Решает, каким станет поле настроек после отправки формы:
//   параметра нет в запросе  -> оставить текущее значение
//   параметр пустой          -> оставить текущее значение
//   параметр == CLEAR_TOKEN  -> очистить
//   иначе                    -> принять новое значение
std::string resolveField(bool present,
                         const std::string& incoming,
                         const std::string& current);

// Чекбоксы приходят строкой "1"/"0". Отсутствие параметра не должно
// молча выключать опцию, поэтому текущее значение сохраняется.
bool resolveFlag(bool present, const std::string& incoming, bool current);

// Числовые поля: непустое и корректное значение из диапазона принимается,
// всё остальное оставляет текущее.
long resolveNumber(bool present, const std::string& incoming, long current,
                   long minValue, long maxValue);

#ifdef ARDUINO
#include <Arduino.h>

// Обёртки для вызова из прошивки — вся логика остаётся в функциях выше.
inline String resolveField(bool present, const String& incoming, const String& current) {
    return String(resolveField(present,
                               std::string(incoming.c_str()),
                               std::string(current.c_str())).c_str());
}

inline bool resolveFlag(bool present, const String& incoming, bool current) {
    return resolveFlag(present, std::string(incoming.c_str()), current);
}

inline long resolveNumber(bool present, const String& incoming, long current,
                          long minValue, long maxValue) {
    return resolveNumber(present, std::string(incoming.c_str()), current, minValue, maxValue);
}
#endif
