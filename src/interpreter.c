/**\file
 * \brief Реализация исполнителя РЕФАЛ (интерпретатора).
 */

#include "library.h"
#include "translator.h"
#include "interpreter.h"
#include <stdbool.h>


static inline
void inconsistence(
      struct refal_message *msg,
      const char *detail,
      intmax_t    err,
      intmax_t    num2)
{
   refal_message(msg, "нарушена программа", detail, err, num2, NULL, NULL);
}

static inline
void runtime_error(
      struct refal_message *msg,
      const char *detail,
      intmax_t    err,
      intmax_t    num2)
{
   refal_message(msg, "ошибка выполнения", detail, err, num2, NULL, NULL);
}


static inline
void *realloc_stack(void **mem, unsigned *size, unsigned *max, size_t element)
{
   void *p = NULL;
   unsigned new_size = *size * 2;
   // Значение индекса (элемента в стеке) кратно меньше размера в байтах,
   // потому переполнение проверяется только для последнего.
   if (new_size > *size) {
      p = refal_realloc(*mem, *size, new_size);
      *mem = p;
      *size = new_size;
      *max = new_size / element;
   }
   return p;
}

/**\details
   Формат функции:

     Ссылка из таблицы символов
                 ↓
   [rf_name][rf_sentence][...][rf_equal][...]
                 ↓
            [rf_sentence][...][rf_equal][...]
                ...
            [rf_sentence][...][rf_equal][...]
                 ↓
   [rf_name следующей ф.] -> Отождествление невозможно.

   [rf_sentence] отсутствует в однострочных исполняемых.
   [rf_equal] отсутствует в «ящиках».

 */
int refal_run_opcodes(
      struct refal_interpreter_config  *cfg,
      struct refal_vm      *vm,
      rf_index             prev,
      rf_index             next,
      rf_index             next_sentence,
      struct refal_message *st)
{
   refal_message_source(st, "исполнитель");
   int r = 0;
   size_t step = 0;

   struct {
      rf_index ip;
      unsigned local;
      rf_index prev;
      rf_index next;
      rf_index result;
   } *stack;
   stack = refal_malloc(cfg->call_stack_size);
   unsigned stack_size = cfg->call_stack_size / sizeof(*stack);
   unsigned sp = 0;

   // Исполняемая функция, для определения имени (обычно, по rf_nop_name).
   struct rtrie_val  fn_name = { .value = next_sentence, .tag = rft_op_code };

   struct {
      // s-переменная или первый элемент e- или t- переменной.
      rf_index s;
      // Последний элемент e- t- переменной, либо 0 (диапазон пуст).
      rf_index last;
   } *var_stack, *var;
   var_stack = refal_malloc(cfg->var_stack_size);
   var = var_stack;
   unsigned vars = cfg->var_stack_size / sizeof(*var_stack);
   // Переменные в блоке нумеруются увеличивающимися монотонно значениями
   // начиная с 0. Используем счётчик как индикатор инициализации переменных.
   unsigned local = 0;

   // В стеке хранится индекс ячейки открывающей структурной скобки,
   // используемый для связывания с парной закрывающей (при копировании
   // e-переменных и формировании результата командами из поля программы).
   rf_index *bracket = refal_malloc(cfg->brackets_stack_size);
   unsigned bracket_max = cfg->brackets_stack_size / sizeof(*bracket);
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
   unsigned evar_max = cfg->locals ? cfg->locals : REFAL_TRANSLATOR_LOCALS_DEFAULT;
   struct {
      rf_index ip;   // откат образца при расширении evar
      rf_index idx;  // откат поля зрения на переменную с данным индексом.
      unsigned bp;   // откат указателя скобок при расширении evar
      rf_index ob;   // предшествующая evar скобка (содержимое стека переписывается!)
   } evar[evar_max];

execute:
   ++step;
   rf_index ip  = next_sentence;    // текущая инструкция в предложении
   rf_index cur = vm->u[prev].next; // текущий элемент в образце
   rf_index result = 0;    // результат формируется между этой и vm->free.
   int ep = -1;      // текущая открытая e-переменная (индекс недействителен исходно)
   unsigned fn_bp = bp;

   // Код операции элемента образца.
   rf_opcode tag = rf_undefined;

   if (vm->free >= rf_index_max - 1) {
      runtime_error(st, "исчерпана память", ip, step);
      r = -1;
      goto cleanup;
   }
sentence:
   // При возможности расширяем e-переменные в текущем предложении.
   // Иначе переходим к следующему образцу.
   if (ep < 0) {
next_sentence:
      cur = vm->u[prev].next;
      assert(ep == -1);
      local = 0;
      // Оставлено для однострочных, поскольку не содержат rf_sentence
      if (!next_sentence)
         goto recognition_impossible;
      ip = next_sentence;
      next_sentence = 0;
      bp = fn_bp;
   } else {
      // Расширяем e-переменную.
      // Если при этом безуспешно дошли до конца образца,
      // откатываем на предыдущую e-переменную.
      while (true) {
         local = evar[ep].idx;
         cur = var[local].last;
         cur = cur ? vm->u[cur].next : var[local].s;
         if (cur != next)
            break;
prev_evar:
         if (--ep < 0)
            goto next_sentence;
      }
      // Если переменную требуется расширить и первый соответствующий её символ
      // в поле зрения:
      // — открывающая структурная скобка, пропускаем до закрывающей;
      // — закрывающая структурная скобка, откатываем к предыдущей переменной.
      switch (vm->u[cur].tag) {
      case rf_closing_bracket:
         goto prev_evar;
      case rf_opening_bracket:
         cur = vm->u[cur].link;
         // TODO накладно проверять, попадает ли индекс в диапазон до next.
         // Задача транслятора это гарантировать. Для случая, когда опкоды
         // получены из другого источника, проверим на принадлежность массиву.
         if (!(cur < vm->size)) {
            goto error_link_out_of_range;
         }
         break;
      default:
         break;
      }
      var[local].last = cur;
      cur = vm->u[cur].next;
      ip = evar[ep].ip;
      bp = evar[ep].bp;
      if (bp) {
         bracket[bp - 1] = evar[ep].ob;
      }
      ++local;
   }

   for (bool fetch; !r ;ip = vm->u[ip].next, cur = fetch ? vm->u[cur].next : cur) {
      fetch = true;
      tag = vm->u[ip].tag;
pattern_match:
      switch (tag) {
      case rf_equal: if (cur != next) goto sentence; break;
      case rf_evar: case rf_sentence: case rf_name:  break;
      default: if (cur == next) goto sentence;       break;
      }

      switch (tag) {
      case rf_undefined: goto error_undefined;

      case rf_char: case rf_number: case rf_identifier:
         if (!rf_svar_equal(vm, cur, ip))
            goto sentence;
         continue;

      case rf_opening_bracket:
         // Данные (link) не совпадают (адресуют разные скобки).
         if (vm->u[cur].tag != rf_opening_bracket)
            goto sentence;
         if (bp == bracket_max &&
            !realloc_stack((void**)&bracket, &cfg->brackets_stack_size, &bracket_max, sizeof(*bracket)))
               goto error_bracket_stack_overflow;
         bracket[bp++] = cur;
         continue;

      case rf_closing_bracket:
         // Данные (link) не совпадают (адресуют разные скобки).
         if (vm->u[cur].tag != rf_closing_bracket)
            goto sentence;
         if (!bp--)
            goto error_parenthesis_unpaired;
         continue;

      case rf_svar:
         if (vm->u[cur].tag == rf_opening_bracket || vm->u[cur].tag == rf_closing_bracket)
            goto sentence;
         [[fallthrough]];
      case rf_tvar: case rf_evar: ;
         rf_index v = vm->u[ip].link;
         assert(!(v > local));
         // При повторных вхождениях переменной сопоставляем с принятым значеним.
         // e-переменная может быть пуста.
         if (v != local) {
            if (tag == rf_svar || (tag == rf_tvar && !var[v].last)) {
               if (!rf_svar_equal(vm, cur, var[v].s))
                  goto sentence;
               continue;
            }
            if (!var[v].last) {
               fetch = false;
               continue;
            }
            // Размер закрытой переменной равен таковому для первого вхождения.
            for (rf_index s = var[v].s; ; s = vm->u[s].next, cur = vm->u[cur].next) {
               rf_opcode t = vm->u[s].tag;
               if (t != vm->u[cur].tag)
                  goto sentence;
               if (t != rf_opening_bracket && t != rf_closing_bracket
                && vm->u[s].data != vm->u[cur].data)
                  goto sentence;
               if (s == var[v].last)
                  break;
            }
            continue;
         }

         ++local;
         if (tag == rf_evar) {
            // TODO аналогичная проверка выполняется и при трансляции.
            if (++ep == evar_max) {
               inconsistence(st, "превышен лимит e-переменных", ep, ip);
               r = -2;
               break;
            }
            evar[ep].idx = v;
         }
         if (&var[v] == &var_stack[vars]) {
            ptrdiff_t nvar = var - var_stack;
            if (!realloc_stack((void**)&var_stack, &cfg->var_stack_size, &vars, sizeof(*var_stack))) {
               runtime_error(st, "стек переменных исчерпан", &var[local] - var_stack, vars);
               r = -1;
               break;
            }
            var = var_stack + nvar;
         }
         // Первое вхождение - присваиваем переменной текущую позицию в образце.
         var[v].s = cur;
         var[v].last = 0;
         if (tag == rf_svar || (tag == rf_tvar && vm->u[cur].tag != rf_opening_bracket))
            continue;
         if (tag == rf_tvar) {
            cur = vm->u[cur].link;
            // TODO см. замечание в sentence.
            if (!(cur < vm->size))
               goto error_link_out_of_range;
            var[v].last = cur;
            continue;
         }
         // e-переменная изначально принимает минимальный (0й размер).
         // Если дальнейшая часть образца не совпадает, размер увеличивается.
         // Текущая позицию используется как граница next.
         // Запоминаем индекс переменной для возможного расширения диапазона
         // (если дальше образец расходится).
         ip  = vm->u[ip].next;
         tag = vm->u[ip].tag;
         evar[ep].ip = ip;
         evar[ep].bp = bp;
         evar[ep].ob = bp ? bracket[bp - 1] : 0;
         // Устанавливаем правую границу, когда она сразу известна
         // и диапазон не пуст.
         switch (tag) {
         case rf_closing_bracket:
            if (!bp)
               goto error_parenthesis_unpaired;
            cur = vm->u[bracket[--bp]].link;
            if (!(cur < vm->size))
               goto error_link_out_of_range;
            if (var[v].s != cur)
               var[v].last = vm->u[cur].prev;
            continue;
         case rf_equal:
            if (cur != next)
               var[v].last = vm->u[next].prev;
            goto equal;
         default:
            goto pattern_match;
         }

      // Начало предложения. Далее следует выражение-образец (возможно, пустое).
      case rf_sentence:
         next_sentence = vm->u[ip].data;
         fetch = false;
         continue;

      case rf_open_function: case rf_execute:
         inconsistence(st, "вычислительная скобка в образце", ip, step);
         r = -2;
         break;

      case rf_name:
recognition_impossible:
         // TODO Раскрутка стека с размещением в поле зрения признака исключения?
         // Делаем результатом что-то похожее на вызов функции с текущим Полем Зрения.
         result = vm->free;
         rf_alloc_value(vm, 0, rf_undefined);
         rf_alloc_command(vm, rf_execute);
         rf_splice_evar_prev(vm, result, vm->free, next);
         rf_alloc_command(vm, rf_open_function);
         rf_alloc_value(vm, rtrie_val_to_raw(fn_name), rf_identifier);
         rf_splice_evar_prev(vm, result, vm->free, vm->u[prev].next);
         if (sp && next != stack[0].next) {
            rf_free_evar(vm, stack[0].prev, stack[0].next);
            rf_splice_evar_prev(vm, prev, next, stack[0].next);
         }
         r = cur;
         break;

      // Начало общего выражения.
      case rf_equal:
equal:   if (fn_bp != bp)
            goto error_parenthesis_unpaired;
         result = vm->free;
         // Для `rf_insert_next()` отделяем свободное пространство от поля зрения.
         rf_alloc_value(vm, 0, rf_undefined);
         break;
      }
      break;
   }

   // Результат
   while (!r) {
      ip  = vm->u[ip].next;
      rf_opcode tag = vm->u[ip].tag;

      switch (tag) {
      case rf_undefined: goto error_undefined;

      case rf_char: case rf_number: case rf_identifier:
         rf_alloc_value(vm, vm->u[ip].data, tag);
         continue;

      case rf_opening_bracket:
         if (bp == bracket_max &&
            !realloc_stack((void**)&bracket, &cfg->brackets_stack_size, &bracket_max, sizeof(*bracket)))
               goto error_bracket_stack_overflow;
         bracket[bp++] = rf_alloc_command(vm, rf_opening_bracket);
         continue;

      case rf_closing_bracket:
         if (!bp)
            goto error_parenthesis_unpaired;
         rf_link_brackets(vm, bracket[--bp], rf_alloc_command(vm, rf_closing_bracket));
         continue;

      case rf_svar: case rf_tvar: case rf_evar: ;
         rf_index v = vm->u[ip].link;
         if (v > local) {
            inconsistence(st, "переменная не определена", ip, step);
            r = -2;
            break;
         }
         if (tag == rf_evar && !var[v].last)
            continue;
         if (tag == rf_svar || (tag == rf_tvar && vm->u[var[v].s].tag != rf_opening_bracket)) {
            //TODO снижает ли это фрагментацию?
            rf_alloc_value(vm, vm->u[var[v].s].data, vm->u[var[v].s].tag);
            continue;
         }
         // Копируем все вхождения кроме последнего (которое переносим).
         // Транслятор отметил копии ненулевым tag2.
         if (!vm->u[ip].tag2) {
            rf_alloc_evar_move(vm, vm->u[var[v].s].prev, vm->u[var[v].last].next);
            continue;
         }
         for (rf_index s = var[v].s; ; s = vm->u[s].next) {
            switch (vm->u[s].tag) {
            case rf_opening_bracket:
               if (bp == bracket_max &&
                  !realloc_stack((void**)&bracket, &cfg->brackets_stack_size, &bracket_max, sizeof(*bracket)))
                     goto error_bracket_stack_overflow;
               bracket[bp++] = rf_alloc_command(vm, rf_opening_bracket);
               break;
            case rf_closing_bracket:
               // Непарная скобка должна быть определена при сопоставлении.
               assert(bp);
               rf_link_brackets(vm, bracket[--bp], rf_alloc_command(vm, rf_closing_bracket));
               break;
            default:
               rf_alloc_value(vm, vm->u[s].data, vm->u[s].tag);
            }
            if (s == var[v].last)
               break;
         }
         continue;

      case rf_equal:
         inconsistence(st, "повторное присваивание", ip, step);
         r = -2;
         break;

      // Открыты вычислительные скобки.
      case rf_open_function:
         if (!(sp < stack_size) &&
            (cfg->call_stack_size * 2 > cfg->call_stack_max
             || !realloc_stack((void**)&stack, &cfg->call_stack_size, &stack_size, sizeof(*stack)))) {
               runtime_error(st, "стек вызовов исчерпан", sp, ip);
               r = -1;
               break;
         }
         stack[sp].prev   = prev;
         stack[sp].next   = next;
         stack[sp].result = result;
         ++sp;
         prev = vm->u[vm->free].prev;
         continue;

      // Закрывающая вычислительная скобка приводит к исполнению функции.
      case rf_execute:
         next = vm->free;
         struct rtrie_val function = rtrie_val_from_raw(vm->u[ip].data);
         fn_name = function;
         switch (function.tag) {
         case rft_module: assert(0);
         case rft_box: case rft_reference: case rft_enum:
            inconsistence(st, "пустая функция", ip, step);
            r = -2;
            continue;
         case rft_undefined:
            goto error_undefined_identifier;
         case rft_machine_code:
            // Функции Mu соответствует индекс 0.
            // Ищем в поле зрения вычислимую функцию
            // либо её имя в глобальном пространстве и вызываем, удаляя из ПЗ.
            // Если очередной функцией является Mu, "исполняем", продолжая поиск.
            if (!function.value) {
Mu:            function = rtrie_find_value_by_tags(vm->rt, rft_op_code, rft_machine_code, vm, prev, next);
               if (function.tag == rft_undefined) {
                  if (function.value == -1)  goto error_link_out_of_range;
                  else if (!function.value)  goto recognition_impossible;
                  else if (function.value)   goto error_undefined_identifier;
               } else if (function.tag == rft_op_code) {
                  fn_name = function;
                  goto execute_byte_code;
               } else if (function.tag == rft_machine_code) {
                  if (!function.value)
                     goto Mu;
                  fn_name = function;
               }
            }
            if (!(function.value < vm->library_size)) {
               inconsistence(st, "библиотечная функция не существует", function.value, ip);
               r = -2;
               continue;
            }
            // TODO при невозможности отождествления функции возвращают
            // `rf_index`, тип без знака. Значение получается из полей next
            // и prev ячеек, где количество значащих разрядов ограничено
            // из-за наличия тега. При имеющейся реализации приведение к int
            // должно всегда попадать в диапазон положительных значений.
            r = vm->library[function.value].function(vm, prev, next);
            if (r > 0) {
               cur = r;
               goto recognition_impossible;
            } else if (r < 0) {
               inconsistence(st, "ошибка среды выполнения", -errno, ip);
               continue;
            }
            // TODO убрать лишние (не изменяются при вызове).
            --sp;
            result = stack[sp].result;
            next   = stack[sp].next;
            prev   = stack[sp].prev;
            continue;
         case rft_op_code:
execute_byte_code:
            // Для хвостовых вызовов транслятор установил признак.
            if (vm->u[ip].tag2 /* == rf_execute */) {
               assert(sp);
               --sp;
               next = stack[sp].next;
               rf_free_evar(vm, stack[sp].prev, next);
               rf_splice_evar_prev(vm, result, vm->free, next);
               rf_free_last(vm);
               if (prev == result)
                  prev = stack[sp].prev;
            } else {
               stack[sp-1].ip = ip;
               stack[sp-1].local = local;
               var += local;
            }
            next_sentence = function.value;
            goto execute;
         }

      case rf_name: case rf_sentence:
         rf_free_evar(vm, prev, next);
         assert(result);
         rf_splice_evar_prev(vm, result, vm->free, next);
         rf_free_last(vm);
         if (!sp--)
            break;
         ip     = stack[sp].ip;
         local  = stack[sp].local;
         prev   = stack[sp].prev;
         next   = stack[sp].next;
         result = stack[sp].result;
         var -= local;
         continue;
      }
      break;
   }

cleanup:
   refal_free(bracket, cfg->brackets_stack_size);
   refal_free(var_stack, cfg->var_stack_size);
   refal_free(stack, cfg->call_stack_size);
   return r;

error_undefined:
   inconsistence(st, "значение не определено", ip, step);
   r = -2;
   goto cleanup;

error_undefined_identifier:
   inconsistence(st, "неопределённая функция", ip, step);
   r = -2;
   goto cleanup;

error_bracket_stack_overflow:
   runtime_error(st, "переполнен стек структурных скобок", bp, ip);
   r = -1;
   goto cleanup;

error_parenthesis_unpaired:
   // TODO аналогичная проверка выполняется и при трансляции.
   inconsistence(st, "непарная закрывающая скобка", ip, step);
   r = -2;
   goto cleanup;

error_link_out_of_range:
   inconsistence(st, "недействительная структурная скобка", cur, vm->size);
   r = -2;
   goto cleanup;
}
