/**\file
 * \brief Реализация РЕФАЛ интерпретатора.
 */

#include "library.h"
#include "translator.h"
#include "interpreter.h"


static inline
void inconsistence(
      struct refal_message *msg,
      const char *detail,
      intmax_t    err,
      intmax_t    num2)
{
   refal_message(msg, "нарушена программа", detail, err, num2, NULL, NULL);
}


enum interpreter_state {
   is_pattern,       ///< Левая часть предложения (до знака =).
   is_expression,    ///< Правая часть предложения (после знака =).
};

/**\details
   Предполагаемый формат функции:
   - Имя функции: маркер со ссылкой на следующее поле (не реализовано).
   - Предложение: маркер со ссылкой на следующее предложение (может отсутствовать).
      - выражение-образец (может отсутствовать).
      - rf_equal — начало общего выражения.
   - rf_complete признак завершения функции.
 */
int refal_interpret_bytecode(
      struct refal_vm      *vm,
      rf_index             prev,
      rf_index             next,
      rf_index             next_sentence,
      struct refal_message *st)
{
   st->source = "интерпретатор";
   size_t step = 0;

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

   // В стеке хранится индекс ячейки открывающей структурной скобки,
   // используемый для связывания с парной закрывающей (при копировании
   // e-переменных и формировании результата командами из поля программы).
   unsigned bracket_max = 5 * stack_size;
   rf_index bracket[bracket_max];
   unsigned bp = 0;

   // Здесь хранятся индексы e-переменных в порядке их появления в образце.
   // Если имеется несколько способов присваивания значений свободным переменным
   // (то есть e-переменным, чей размер изначально не известен), то выбирается
   // такой способ, при котором самая левая принимает наиболее короткое значение.
   // Для чего при первом вхождении размер переменной (в `var_stack[]`) задаётся
   // пустым, а индекс и адрес инструкции сохраняются в нижеследующем стеке.
   // В случае, если в текущих границах e-переменных сопоставление не происходит,
   // границы переменной с вершины стека (то есть самой правой) увеличиваются
   // и образец проверяется повторно. Если же увеличение размера правой
   // переменной не приводит к сопоставлению, расширяем предыдущую, начав
   // формирование последующих заново. И так далее, рекурсивно до начала стека.
   unsigned evar_max = REFAL_TRANSLATOR_LOCALS_DEFAULT;
   struct {
      rf_index ip;   // откат образца при расширении evar
      rf_index idx;  // откат поля зрения на переменную с данным индексом.
   } evar[evar_max];

execute:
   ++step;
   enum interpreter_state state = is_pattern;
   rf_index ip  = next_sentence;    // текущая инструкция в предложении
   rf_index cur = vm->u[prev].next; // текущий элемент в образце
   rf_index result = 0;    // результат формируется между этой и vm->free.
   int ep = -1;      // текущая открытая e-переменная (индекс недействителен исходно)

   // Тип текущего элемента образца.
   rf_type tag = rf_undefined;

sentence:
   // При возможности расширяем e-переменные в текущем предложении.
   // Иначе переходим к следующему образцу.
   if (ep >= 0) {
      // Если при расширении правой переменной безуспешно дошли до конца образца,
      // откатываем на предыдущую e-переменную.
      while (var[evar[ep].idx].next == next) {
         if (--ep < 0)
            goto next_sentence;
      }
      local = evar[ep].idx;
      cur = vm->u[var[local].next].next;
      // Пропуск до закрытой скобки можно считать преждевременной оптимизацией,
      // но иначе придётся вводить лишнюю сущность для проверки баланса скобок.
      if (tag != rf_opening_bracket && vm->u[cur].tag == rf_opening_bracket) {
         cur = vm->u[cur].link;
         // TODO накладно проверять, попадает ли индекс в диапазон до next.
         // Задача транслятора это гарантировать. Для случая, когда байт-код
         // получен из другого источника, проверим на принадлежность массиву.
         if (!(cur < vm->size)) {
            goto error_link_out_of_range;
         }
         cur = vm->u[cur].next;
      }
      var[local].next = cur;
      ip = evar[ep].ip;
      ++local;
   } else {
next_sentence:
      ep = -1;
      local = 0;
      ip = next_sentence;
   }
   while (1) {
      // Индекс текущей переменной. Вынесен сюда, поскольку
      // обработка t-переменных совмещена с таковой для s- и e-.
      rf_index v = -1;

      // При входе в функцию, tag первой ячейки:
      // - rf_equal — для простых функций.
      // - [не определено] — для обычных функций несколько предложений в {блоке}.
      tag = vm->u[ip].tag;
      switch (tag) {
      case rf_undefined:
         inconsistence(st, "значение не определено", ip, step);
         goto error;

      case rf_char:
      case rf_number:
      case rf_atom:
      case rf_identifier:
         switch (state) {
         case is_pattern:
            // При наличии данных в Поле Зрения сравниваем с образцом.
            if (cur == next || !rf_svar_equal(vm, cur, ip)) {
               goto sentence;
            }
            cur = vm->u[cur].next;
            goto next;
         case is_expression:
            rf_alloc_value(vm, vm->u[ip].data, tag);
            goto next;
         }

      case rf_opening_bracket:
         switch (state) {
         case is_pattern:
            // Данные (link) не совпадают (адресуют разные скобки).
            if (cur == next || vm->u[cur].tag != rf_opening_bracket) {
               goto sentence;
            }
            cur = vm->u[cur].next;
            goto next;
         case is_expression:
            if (bp == bracket_max) {
               goto error_bracket_stack_overflow;
            }
            bracket[bp++] = rf_alloc_command(vm, rf_opening_bracket);
            goto next;
         }

      case rf_closing_bracket:
         switch (state) {
         case is_pattern:
            if (cur == next || vm->u[cur].tag != rf_closing_bracket) {
               goto sentence;
            }
            cur = vm->u[cur].next;
            goto next;
         case is_expression:
            if (!bp) {
               goto error_parenthesis_unpaired;
            }
            rf_link_brackets(vm, bracket[--bp], rf_alloc_command(vm, rf_closing_bracket));
            goto next;
         }

      case rf_svar:
      case rf_tvar:
         v = vm->u[ip].link;
         switch (state) {
         case is_pattern:
            if (cur == next) {
               goto sentence;
            }
            // Скобка допустима только для термов (t-переменных)
            const rf_type t = vm->u[cur].tag;
            if (t == rf_closing_bracket) {
               goto error_parenthesis_unpaired;
            }
            // При первом вхождении присваиваем переменной значение образца.
            // При повторных сопоставляем.
            if (v >= local) {
               assert(local == v);  // TODO убрать, заменив условие выше.
               if (&var[local] == &var_stack[vars]) {
                  goto error_var_stack_overflow;
               }
               var[v].s = cur;
               if (t == rf_opening_bracket) {
                  if (tag == rf_svar)
                     goto sentence;
                  cur = vm->u[cur].link;
                  // TODO см. замечание в sentence.
                  if (!(cur < vm->size)) {
                     goto error_link_out_of_range;
                  }
                  assert(vm->u[cur].tag == rf_closing_bracket);
                  var[v].next = vm->u[cur].next;
               }
               ++local;
            } else if (t == rf_opening_bracket) {
               assert(tag == rf_tvar);
               goto evar_compare;
            } else if (!rf_svar_equal(vm, cur, var[v].s)) {
               goto sentence;
            }
            cur = vm->u[cur].next;
            goto next;
         case is_expression:
            if (vm->u[ip].link > local) {
               goto error_undefined_variable;
            }
            const rf_index sval = var[v].s;
            if (vm->u[sval].tag == rf_opening_bracket) {
               assert(tag == rf_tvar);
               goto evar_express;
            }
            rf_alloc_value(vm, vm->u[sval].data, vm->u[sval].tag);
            goto next;
         }

      case rf_evar:
         v = vm->u[ip].link;
         switch (state) {
         // e-переменная изначально принимает минимальный (0й размер).
         // Если дальнейшая часть образца не совпадает, размер увеличивается.
         case is_pattern:
            // При первом вхождении присваиваем переменной текущую позицию в
            // образце (как границу next) и запоминаем индекс переменной для
            // возможного расширения диапазона (если дальше образец расходится).
            // При повторных — сопоставляем.
            if (v >= local) {
               if (++ep == evar_max) {
                  // TODO аналогичная проверка выполняется и при трансляции.
                  inconsistence(st, "превышен лимит e-переменных", ep, ip);
                  goto error;
               }
               assert(local == vm->u[ip].link);   // TODO убрать, заменив условие выше.
               evar[ep].idx = vm->u[ip].link;
               if (&var[local] == &var_stack[vars]) {
                  goto error_var_stack_overflow;
               }
               var[local].s    = cur;
               var[local].next = cur;
               ++local;
               evar[ep].ip = vm->u[ip].next;
               goto next;  // cur не меняется, исходно диапазон пуст.
            } else {
evar_compare:
               // Размер закрытой переменной равен таковому для первого вхождения.
               for (rf_index s = var[v].s; s != var[v].next; s = vm->u[s].next) {
                  rf_type t = vm->u[s].tag;
                  if (t != vm->u[cur].tag)
                     goto sentence;
                  if (t != rf_opening_bracket && t != rf_closing_bracket
                   && vm->u[s].data != vm->u[cur].data)
                     goto sentence;
                  cur = vm->u[cur].next;
               }
               goto next;
            }
         case is_expression:
            if (vm->u[ip].link >= local) {
               goto error_undefined_variable;
            }
evar_express:
            // Копируем все вхождения кроме последнего (которое переносим).
            // Транслятор отметил копии ненулевым tag2.
            if (vm->u[ip].tag2) {
               for (rf_index s = var[v].s; s != var[v].next; s = vm->u[s].next) {
                  rf_type t = vm->u[s].tag;
                  switch (t) {
                  case rf_opening_bracket:
                     if (bp == bracket_max) {
                        goto error_bracket_stack_overflow;
                     }
                     bracket[bp++] = rf_alloc_command(vm, rf_opening_bracket);
                     break;
                  case rf_closing_bracket:
                     // Непарная скобка должна быть определена при сопоставлении.
                     assert(bp);
                     rf_link_brackets(vm, bracket[--bp], rf_alloc_command(vm, rf_closing_bracket));
                     break;
                  default:
                     rf_alloc_value(vm, vm->u[s].data, t);
                  }
               }
            } else {
               if (var[v].s != var[v].next)
                  rf_alloc_evar_move(vm, vm->u[var[v].s].prev, var[v].next);
            }
            goto next;
         }

      // Начало предложения. Далее следует выражение-образец (возможно, пустое).
      case rf_sentence:
         switch (state) {
         case is_pattern:
            next_sentence = vm->u[ip].data;
            cur = vm->u[prev].next;
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
            prev = vm->u[vm->free].prev;
            goto next;
         }

      case rf_execute_close:
         switch (state) {
         case is_pattern:
            goto error_execution_bracket;
         case is_expression:
            next = vm->free;
            struct rtrie_val function = rtrie_val_from_raw(vm->u[ip].data);
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
next: ip = vm->u[ip].next;
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

error_bracket_stack_overflow:
   inconsistence(st, "переполнен стек структурных скобок", bp, ip);
   goto error;

error_parenthesis_unpaired:
   // TODO аналогичная проверка выполняется и при трансляции.
   inconsistence(st, "непарная закрывающая скобка", ip, step);
   goto error;

error_link_out_of_range:
   inconsistence(st, "недействительная структурная скобка", cur, vm->size);
   goto error;
}
