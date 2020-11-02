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
   rtrie_alloc(&it->ids, 100);
   refal_vm_init(&it->vm, 500);
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
      rf_index             next_sentence, ///< Начальная инструкция.
      struct refal_message *st)
{
   st->source = "интерпретатор";
   size_t step = 0;

   // Границы поля зрения:
   rf_index next = vm->free;
   rf_index prev = vm->cell[next].prev;

   unsigned stack_size = 100;
   struct {
      rf_index ip;
      unsigned local;
      rf_index prev;
      rf_index next;
      rf_index result;
   } stack[stack_size];
   unsigned sp = 0;

   unsigned vars = 5 * stack_size;
   struct {
      rf_index s;
      rf_index next;
   } var_stack[vars], *var = var_stack;
   // Переменные в блоке нумеруются увеличивающимися монотонно значениями
   // начиная с 0. Используем счётчик как индикатор инициализации переменных.
   unsigned local = 0;

execute:
   ++step;
   enum interpreter_state state = is_pattern;
   rf_index ip  = next_sentence;       // текущая инструкция в предложении
   rf_index cur = vm->cell[prev].next; // текущий элемент в образце
   rf_index evar_ip = 0;   // откат образца при расширении evar
   rf_index evar = vars;   // откат поля зрения (изначально недействителен).
   rf_index result = 0;    // результат формируется между этой и vm->free.
sentence:
   // При возможности расширяем последнюю e-переменную в текущем предложении.
   // Иначе переходим к следующему образцу.
   if (evar < vars && var[evar].next != next) {
      cur = vm->cell[var[evar].next].next;
      var[evar].next = cur;
      ip = evar_ip;
   } else {
      evar = vars;
      local = 0;
      ip = next_sentence;
   }
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
      case rf_identifier:
      case rf_opening_bracket:
      case rf_closing_bracket:
         switch (state) {
         case is_pattern:
            // При наличии данных в Поле Зрения сравниваем с образцом.
            if (cur == next || !rf_svar_equal(vm, cur, ip)) {
               goto sentence;
            }
            cur = vm->cell[cur].next;
            goto next;
         case is_expression:
            rf_alloc_value(vm, vm->cell[ip].data, tag);
            goto next;
         }

      case rf_svar:
         switch (state) {
         case is_pattern:
            if (cur == next) {
               goto sentence;
            }
            const rf_index svar = vm->cell[ip].link;
            // При первом вхождении присваиваем переменной значение образца.
            // При повторных сопоставляем.
            if (svar >= local) {
               assert(local == svar);   // TODO убрать, заменив условие выше.
               if (&var[local] == &var_stack[vars]) {
                  goto error_var_stack_overflow;
               }
               var[svar].s = cur;
               ++local;
            } else if (!rf_svar_equal(vm, cur, var[svar].s)) {
               goto sentence;
            }
            cur = vm->cell[cur].next;
            goto next;
         case is_expression:
            if (vm->cell[ip].link > local) {
               goto error_undefined_variable;
            }
            const rf_index sval = var[vm->cell[ip].link].s;
            rf_alloc_value(vm, vm->cell[sval].data, vm->cell[sval].tag);
            goto next;
         }

      case rf_evar:
         switch (state) {
         // e-переменная изначально принимает минимальный (0й размер).
         // Если дальнейшая часть образца не совпадает, размер увеличивается.
         case is_pattern:
            // При первом вхождении присваиваем переменной текущую позицию в
            // образце (как границу next) и запоминаем индекс переменной для
            // возможного расширения диапазона (если дальше образец расходится).
            // При повторных — сопоставляем.
            if (vm->cell[ip].link >= local) {
               evar = vm->cell[ip].link;
               assert(local == evar);   // TODO убрать, заменив условие выше.
               if (&var[local] == &var_stack[vars]) {
                  goto error_var_stack_overflow;
               }
               var[evar].s    = cur;
               var[evar].next = cur;
               ++local;
               evar_ip = vm->cell[ip].next;
               goto next;  // cur не меняется, исходно диапазон пуст.
            } else {
               assert(0);
               //if (!rf_evar_equal(vm, cur, next, var[evar-1], var[evar]))

            }
         case is_expression:
evar_expression:
            if (vm->cell[ip].link >= local) {
               goto error_undefined_variable;
            }
            // TODO копировать при повторном вхождении.
            const rf_index eidx = vm->cell[ip].link;
            if (var[eidx].s != var[eidx].next) {
               rf_alloc_evar_move(vm, vm->cell[var[eidx].s].prev, var[eidx].next);
            }
            goto next;
         }

      case rf_evar_copy:
         switch (state) {
         case is_pattern:
            inconsistence(st, "rf_evar_copy в образце", ip, step);
            goto error;
         case is_expression:
            assert(0);
            goto evar_expression;
         }

      // Начало предложения. Далее следует выражение-образец (возможно, пустое).
      case rf_sentence:
         switch (state) {
         case is_pattern:
            next_sentence = vm->cell[ip].data;
            cur = vm->cell[prev].next;
            goto next;
         case is_expression:
            goto complete;
         }

      // Начало общего выражения.
      case rf_equal:
         switch (state) {
         case is_pattern:
            if (cur != next) {
               goto sentence;
            }
            state = is_expression;
            result = vm->free;
            // Для `rf_insert_next()` отделяем свободное пространство от поля зрения.
            rf_alloc_value(vm, 0, rf_undefined);
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
            stack[sp].prev   = prev;
            stack[sp].next   = next;
            stack[sp].result = result;
            ++sp;
            prev = vm->cell[vm->free].prev;
            goto next;
         }

      case rf_execute_close:
         switch (state) {
         case is_pattern:
            goto error_execution_bracket;
         case is_expression:
            next = vm->free;
            struct rtrie_val function = rtrie_val_from_raw(vm->cell[ip].data);
            switch (function.tag) {
            case rft_enum:
               inconsistence(st, "пустая функция", ip, step);
               goto error;
            case rft_undefined:
               inconsistence(st, "неопределённая функция", ip, step);
               goto error;
            case rft_machine_code:
               if (!(function.value < refal_library_size)) {
                  inconsistence(st, "библиотечная функция не существует", function.value, ip);
                  goto error;
               }
               refal_library_call(vm, prev, next, function.value);
               // TODO убрать лишние (не изменяются при вызове).
               --sp;
               result = stack[sp].result;
               next   = stack[sp].next;
               prev   = stack[sp].prev;
               goto next;
            case rft_byte_code:
               if (!(sp < stack_size)) {
                  inconsistence(st, "стек вызовов исчерпан", sp, ip);
                  goto error;
               }
               stack[sp-1].ip = ip;
               stack[sp-1].local = local;
               var += local;
               next_sentence = function.value;
               goto execute;
            }
         }

      case rf_complete:
         switch (state) {
         case is_pattern:
            // Для `rf_insert_next()` отделяем свободное пространство от поля зрения.
            rf_alloc_value(vm, 0, rf_undefined);
            // TODO Ошибки в байт-коде нет. Реализовать вывод текущего состояния.
            rf_insert_prev(vm, next, rf_alloc_char(vm, '\n'));
            rf_insert_prev(vm, next, rf_alloc_atom(vm, "Отождествление невозможно. "));
            goto stop;
         case is_expression:
complete:   rf_free_evar(vm, prev, next);
            assert(result);
            rf_splice_evar_prev(vm, result, vm->free, next);
            rf_free_last(vm);
            if (sp--) {
               ip     = stack[sp].ip;
               local  = stack[sp].local;
               prev   = stack[sp].prev;
               next   = stack[sp].next;
               result = stack[sp].result;
               var -= local;
               goto next;
            }
            goto stop;
         }
      }
next: ip = vm->cell[ip].next;
   }
stop:
   if (!rf_is_evar_empty(vm, prev, next)) {
      // Для `rf_insert_next()` отделяем свободное пространство от поля зрения.
      rf_alloc_value(vm, 0, rf_undefined);
      rf_index n = rf_alloc_atom(vm, "Поле зрения:");
      rf_alloc_char(vm, '\n');
      rf_insert_next(vm, prev, n);
      Prout(vm, prev, next);
   }
   return 0;

error:
   return -1;

error_execution_bracket:
   inconsistence(st, "вычислительная скобка в образце", ip, step);
   goto error;

error_undefined_variable:
   inconsistence(st, "переменная не определена", ip, step);
   goto error;

error_var_stack_overflow:
   inconsistence(st, "стек переменных исчерпан", &var[local] - var_stack, vars);
   goto error;
}

/**
 * Переводит исходный текст в байт-код для интерпретатора.
 * При этом заполняется таблица символов.
 * \return количество необработанных байт исходного файла. 0 при успехе.
 */
static
size_t translate(
      struct refal_interpreter   *ri,
      const char           *name,      ///< имя файла с исходным текстом.
      struct refal_message *st)
{
   st->source = name;
   size_t source_size = 0;
   const char *source = mmap_file(name, &source_size);
   if (source == MAP_FAILED) {
      if (st)
         critical_error(st, "исходный текст недоступен", -errno, source_size);
      return -1;
   }
   size_t r = refal_parse_text(&ri->ids, &ri->vm, source, &source[source_size], st);
   munmap((void*)source, source_size);
   return source_size - r;
}


int main(int argc, char **argv)
{
   struct refal_message status = {
         .handler = refal_message_print,
         .source  = "Интерпретатор РЕФАЛ",
   };

   if (argc != 2) {
      critical_error(&status, "укажите одно имя файла с исходным текстом", argc, 0);
      return 0;
   }

   struct refal_interpreter refint;
   refal_interpreter_init(&refint);

   if (rtrie_check(&refint.ids, &status) && refal_vm_check(&refint.vm, &status)) {

      refal_import(&refint.ids, library, &status);

      translate(&refint, argv[1], &status);

      struct rtrie_val entry = rtrie_get_value(&refint.ids, "go");
      if (entry.tag != rft_byte_code) {
         critical_error(&status, "не определена функция go", entry.value, 0);
      } else {
         interpret(&refint.vm, entry.value, &status);
      }
   }
   refal_interpreter_free(&refint);
   return 0;
}
