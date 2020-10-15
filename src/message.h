/**\file
 * \brief Работа с сообщениями (ошибки, предупреждения).
 *
 * \defgroup messages Сообщения РЕФАЛ.
 */

#pragma once

#include <stdint.h>

struct refal_message;

/**
 * Обрабатывает сообщения.
 */
typedef void refal_message_handler(
      struct refal_message *message ///< Параметры сообщения.
      );

/**\ingroup messages
 *
 * Содержит подробности сообщения и обработчик (для вывода).
 */
struct refal_message {
   refal_message_handler   *handler;   ///< Обработчик
   void                    *context;   ///< Контекст обработчика (канал вывода).
   const char *source;  ///< Источник ошибки (имя файла с исходным текстом).
   const char *type;    ///< Тип сообщения (ошибка, предупреждение).
   const char *detail;  ///< Подробное описание.
   intmax_t    line;    ///< Номер строки либо иная характеристика (errno).
   intmax_t    position;///< Позиция в строке либо иная характеристика.
   const char *begin;   ///< Начало участка ошибочного текста.
   const char *end;     ///< Адрес за границей участка текста (обращение недопустимо).
};

/**\ingroup messages
 *
 * Выводит сообщение в поток вывода.
 */
refal_message_handler refal_message_print;

/**
 * Конструирует `struct refal_message`, за исключением поля `source`.
 * Вызывает обработчик при наличии.
 */
static inline
void refal_message(
      struct refal_message *msg, ///< Адрес объекта либо NULL.
      const char *type,
      const char *detail,
      intmax_t    line,
      intmax_t    position,
      const char *begin,
      const char *end)
{
   if (msg) {
      msg->type   = type;
      msg->detail = detail;
      msg->begin  = begin;
      msg->end    = end;
      msg->line   = line;
      msg->position  = position;
      if (msg->handler)
         msg->handler(msg);
   }
}

static inline
void critical_error(
      struct refal_message *msg,
      const char *detail,
      intmax_t    err,
      intmax_t    num2)
{
   refal_message(msg, "критическая ошибка", detail, err, num2, NULL, NULL);
}

static inline
void syntax_error(
      struct refal_message *msg,
      const char *detail,
      intmax_t    line,
      intmax_t    position,
      const char *begin,
      const char *end)
{
   refal_message(msg, "ошибка", detail, line, position, begin, end);
}

static inline
void warning(
      struct refal_message *msg,
      const char *detail,
      intmax_t    line,
      intmax_t    position,
      const char *begin,
      const char *end)
{
   refal_message(msg, "предупреждение", detail, line, position, begin, end);
}


