/**\file
 * \brief Реализация РЕФАЛ интерпретатора.
 */

#include <error.h>
#include "library.h"
#include "rtrie.h"
#include "refal.h"

size_t refal_parse_text(
      struct refal_trie    *ids,
      struct refal_vm      *vm,
      const char           *begin,
      const char           *end,
      struct refal_message *st
      );


static inline
void inconsistence(
      struct refal_message *msg,
      const char *detail,
      intmax_t    err,
      intmax_t    num2)
{
   refal_message(msg, "нарушена программа", detail, err, num2, NULL, NULL);
}


struct refal_interpreter {
   struct refal_trie ids;  ///< Идентификаторы.
   struct refal_vm   vm;   ///< Байт-код и поле зрения.
};

static inline
void refal_interpreter_init(
      struct refal_interpreter   *it)
{
   rtrie_alloc(&it->ids, 25);
   refal_vm_init(&it->vm, 100);
}

static inline
void refal_interpreter_free(
      struct refal_interpreter   *it)
{
   rtrie_free(&it->ids);
   refal_vm_free(&it->vm);
}


static inline
rtrie_index refal_import(
      struct refal_trie                      *rt,
      const struct refal_import_descriptor   *lib,
      struct refal_message                   *ectx)
{
   const rtrie_index free = rt->free;
   for (int ordinal = 0; lib->name; ++lib, ++ordinal) {
      const char *p = lib->name;
      assert(p);
      rtrie_index idx = rtrie_insert_first(rt, *p++);
      while (*p) {
         idx = rtrie_insert_next(rt, idx, *p++);
      }
      rt->n[idx].val.tag   = rft_machine_code;
      rt->n[idx].val.value = ordinal;
   }
   return rt->free - free;
}


enum interpreter_state {
   is_pattern,       ///< Левая часть предложения (до знака =).
   is_expression,    ///< Правая часть предложения (после знака =).
};

/**
 * Выполняет функцию РЕФАЛ.
 *
   Предполагаемый формат функции:
   - Имя функции: маркер со ссылкой на следующее поле (может отсутствовать).
   - Предложение: маркер со ссылкой на следующее предложение (может отсутствовать).
      - выражение-образец (может отсутствовать).
      - rf_equal — начало общего выражения.
   - rf_complete признак завершения функции.
 */
static
int interpret(
      struct refal_vm      *vm,
      rf_index             ip,   ///< Начальная инструкция.
      struct refal_message *st)
{
   st->source = "интерпретатор";
   size_t step = 0;

   // Границы поля зрения:
   rf_index next = vm->free;
   rf_index prev = vm->cell[next].prev;

   // Размещение новых значений в поле зрения происходит по одному, начиная
   // с первой свободной ячейки vm->free. По завершению размещения
   // осуществляется вставка, связывающая новые ячейки в список.
   rf_index first_new = 0;
   // Для `rf_insert_next()` отделяем свободное пространство от поля зрения.
   rf_alloc_value(vm, 0, rf_undefined);

   struct rtrie_val function = {};

   unsigned stack_size = 100;
   rf_index stack[stack_size];
   unsigned sp = 0;

execute:
   ++step;
   enum interpreter_state state = is_pattern;
   rf_index next_sentence = 0;
   while (1) {
      // При входе в функцию, tag первой ячейки:
      // - rf_equal — для простых функций.
      // - [не определено] — для обычных функций несколько предложений в {блоке}.
      rf_type tag = vm->cell[ip].tag;
      switch (tag) {
      case rf_undefined:
         inconsistence(st, "значение не определено", ip, step);
         goto error;

      case rf_char:
      case rf_number:
      case rf_atom:
      case rf_opening_bracket:
      case rf_closing_bracket:
         switch (state) {
         case is_pattern:
            assert(0); // TODO обработать образец
         case is_expression:
            rf_alloc_value(vm, vm->cell[ip].data, tag);
            goto next;
         }
         break;

      // Начало предложения. Далее следует выражение-образец (возможно, пустое).
      case rf_sentence:
         switch (state) {
         case is_pattern:
            next_sentence = vm->cell[ip].data;
            goto next;
         case is_expression:
            goto complete;
         }

      // Начало общего выражения.
      case rf_equal:
         switch (state) {
         case is_pattern:
            assert(rf_is_evar_empty(vm, prev, next)); // TODO обработать образец
            state = is_expression;
            first_new = vm->free;
            goto next;
         case is_expression:
            inconsistence(st, "повторное присваивание", ip, step);
            goto error;
         }

      // Открыты вычислительные скобки.
      case rf_execute:
         switch (state) {
         case is_pattern:
            goto error_execution_bracket;
         case is_expression:
            function = rtrie_val_from_raw(vm->cell[ip].data);
            goto next;
         }

      case rf_execute_close:
         switch (state) {
         case is_pattern:
            goto error_execution_bracket;
         case is_expression:
            if (first_new != vm->free) {
               rf_insert_next(vm, prev, first_new);
            }
            switch (function.tag) {
            case rft_undefined:
               inconsistence(st, "неопределённая функция", ip, step);
               goto error;
            case rft_machine_code:
               if (!(function.value < refal_library_size)) {
                  inconsistence(st, "библиотечная функция не существует", function.value, ip);
                  goto error;
               }
               refal_library_call(vm, prev, next, function.value);
               goto next;
            case rft_byte_code:
               if (!(sp < stack_size)) {
                  inconsistence(st, "стек вызовов исчерпан", sp, ip);
                  goto error;
               }
               stack[sp++] = ip;
               ip = function.value;
               goto execute;
            }
         }

      case rf_complete:
         switch (state) {
         case is_pattern:
            inconsistence(st, "отсутствует общее выражение", ip, step);
            goto error;
         case is_expression:
complete:   if (sp) {
               ip = stack[--sp];
               goto next;
            }
            goto stop;
         }
      }
next: ip = vm->cell[ip].next;
   }
stop:

   assert(rf_is_evar_empty(vm, prev, next)); // TODO вывести поле зрения.
   return 0;

error:
   return -1;

error_execution_bracket:
   inconsistence(st, "вычислительная скобка в образце", ip, step);
   goto error;
}

static
int process_file(
      struct refal_interpreter   *ri,
      const char           *name,
      struct refal_message *st)
{
   st->source = name;

   size_t source_size = 0;
   const char *source = mmap_file(name, &source_size);
   if (source == MAP_FAILED) {
      if (st)
         critical_error(st, "исходный текст недоступен", -errno, source_size);
   } else {
      size_t r = refal_parse_text(&ri->ids, &ri->vm, source, &source[source_size], st);
      assert(r == source_size);

      struct rtrie_val entry = rtrie_get_value(&ri->ids, "go");
      if (entry.tag != rft_byte_code) {
         critical_error(st, "не определена функция go", entry.value, 0);
      } else {
         interpret(&ri->vm, entry.value, st);
      }

   }

   return 0;
}


int main(int argc, char **argv)
{
   struct refal_message status = {
         .handler = refal_message_print,
         .source  = argv[0],
   };

   struct refal_interpreter refint;
   refal_interpreter_init(&refint);

   if (rtrie_check(&refint.ids, &status) && refal_vm_check(&refint.vm, &status)) {

      refal_import(&refint.ids, library, &status);

      struct rtrie_val val;
      val = rtrie_get_value(&refint.ids, "Prout");
      assert(val.tag);
      val = rtrie_get_value(&refint.ids, "Print");
      assert(val.tag);

      process_file(&refint, "tests/function.ref", &status);

   }
   return 0;
}
