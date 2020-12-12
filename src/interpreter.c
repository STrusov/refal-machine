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
void *realloc_stack(void **mem, unsigned *size)
{
   void *p = NULL;
   unsigned new_size = *size * 2;
   // Значение индекса (элемента в стеке) кратно меньше размера в байтах,
   // потому переполнение проверяется только для последнего.
   if (new_size > *size) {
      p = refal_realloc(*mem, *size, new_size);
      *mem = p;
      *size = new_size;
   }
   return p;
}

/**\details
   Предполагаемый формат функции:
   - Имя функции: маркер со ссылкой на следующее поле (не реализовано).
   - Предложение: маркер со ссылкой на следующее предложение.
      - выражение-образец (может отсутствовать).
      - rf_equal — начало общего выражения.
   - rf_complete признак завершения функции.
 */
int refal_interpret_bytecode(
      struct refal_interpreter_config  *cfg,
      struct refal_vm      *vm,
      rf_index             prev,
      rf_index             next,
      rf_index             next_sentence,
      struct refal_message *st)
{
   refal_message_source(st, "интерпретатор");
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

   // Тип текущего элемента образца.
   rf_type tag = rf_undefined;

   if (vm->free >= rf_index_max - 1) {
      runtime_error(st, "исчерпана память", ip, step);
      goto error;
   }
sentence:
   // При возможности расширяем e-переменные в текущем предложении.
   // Иначе переходим к следующему образцу.
   if (ep < 0) {
next_sentence:
      cur = vm->u[prev].next;
      ep = -1;
      local = 0;
      ip = next_sentence;
      bp = fn_bp;
   } else {
      // Расширяем e-переменную.
      // Если при этом безуспешно дошли до конца образца,
      // откатываем на предыдущую e-переменную.
      while (1) {
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
         // Задача транслятора это гарантировать. Для случая, когда байт-код
         // получен из другого источника, проверим на принадлежность массиву.
         if (!(cur < vm->size)) {
            goto error_link_out_of_range;
         }
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
   goto pattern;

pattern_continue:
   cur = vm->u[cur].next;
pattern_next_instruction:
   ip = vm->u[ip].next;
pattern:
   tag = vm->u[ip].tag;

   // Индекс текущей переменной. Вынесен сюда, поскольку
   // обработка t-переменных совмещена с таковой для s- и e-.
   rf_index v = -1;

pattern_match:
   switch (tag) {
   case rf_undefined:
      goto error_undefined;

   case rf_char:
   case rf_number:
   case rf_atom:
   case rf_identifier:
      // При наличии данных в Поле Зрения сравниваем с образцом.
      if (cur == next || !rf_svar_equal(vm, cur, ip)) {
         goto sentence;
      }
      goto pattern_continue;

   case rf_opening_bracket:
      // Данные (link) не совпадают (адресуют разные скобки).
      if (cur == next || vm->u[cur].tag != rf_opening_bracket) {
         goto sentence;
      }
      if (bp == bracket_max) {
         if (!realloc_stack((void**)&bracket, &cfg->brackets_stack_size)) {
            goto error_bracket_stack_overflow;
         }
         bracket_max = cfg->brackets_stack_size / sizeof(*bracket);
      }
      bracket[bp++] = cur;
      goto pattern_continue;

   case rf_closing_bracket:
      // Данные (link) не совпадают (адресуют разные скобки).
      if (cur == next || vm->u[cur].tag != rf_closing_bracket) {
         goto sentence;
      }
      if (!bp) {
         goto error_parenthesis_unpaired;
      }
      --bp;
      goto pattern_continue;

   case rf_svar:
   case rf_tvar:
      v = vm->u[ip].link;
      if (cur == next) {
         goto sentence;
      }
      // Открытая скобка допустима только для термов (t-переменных)
      // и проверяется дальше. Закрытая всегда не подходит под образец.
      const rf_type t = vm->u[cur].tag;
      if (t == rf_closing_bracket) {
         goto sentence;
      }
      // При первом вхождении присваиваем переменной значение образца.
      // При повторных сопоставляем.
      if (v >= local) {
         assert(local == v);  // TODO убрать, заменив условие выше.
         if (&var[local] == &var_stack[vars]) {
            ptrdiff_t nvar = var - var_stack;
            if (!realloc_stack((void**)&var_stack, &cfg->var_stack_size)) {
               goto error_var_stack_overflow;
            }
            var = var_stack + nvar;
            vars = cfg->var_stack_size / sizeof(*var_stack);
         }
         var[v].s = cur;
         var[v].last = 0;
         if (t == rf_opening_bracket) {
            if (tag == rf_svar)
               goto sentence;
            cur = vm->u[cur].link;
            // TODO см. замечание в sentence.
            if (!(cur < vm->size)) {
               goto error_link_out_of_range;
            }
            assert(vm->u[cur].tag == rf_closing_bracket);
            var[v].last = cur;
         }
         ++local;
      } else if (t == rf_opening_bracket && tag == rf_tvar) {
         goto evar_compare;
      } else if (t == rf_opening_bracket) {
         goto sentence;
      } else if (!rf_svar_equal(vm, cur, var[v].s)) {
         goto sentence;
      }
      goto pattern_continue;

   case rf_evar:
      v = vm->u[ip].link;
      // e-переменная изначально принимает минимальный (0й размер).
      // Если дальнейшая часть образца не совпадает, размер увеличивается.
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
         assert(local == v);  // TODO убрать, заменив условие выше.
         ++local;
         evar[ep].idx = v;
         if (&var[v] == &var_stack[vars]) {
            ptrdiff_t nvar = var - var_stack;
            if (!realloc_stack((void**)&var_stack, &cfg->var_stack_size)) {
               goto error_var_stack_overflow;
            }
            var = var_stack + nvar;
            vars = cfg->var_stack_size / sizeof(*var_stack);
         }
         var[v].s = cur;
         var[v].last = 0;
         ip  = vm->u[ip].next;
         tag = vm->u[ip].tag;
         evar[ep].ip = ip;
         evar[ep].bp = bp;
         evar[ep].ob = bp ? bracket[bp - 1] : 0;
         // Устанавливаем правую границу, когда она сразу известна
         // и диапазон не пуст.
         switch (tag) {
         case rf_closing_bracket:
            if (!bp) {
               goto error_parenthesis_unpaired;
            }
            cur = vm->u[bracket[--bp]].link;
            if (!(cur < vm->size)) {
               goto error_link_out_of_range;
            }
            if (var[v].s != cur)
               var[v].last = vm->u[cur].prev;
            goto pattern_continue;
         case rf_equal:
            if (cur != next)
               var[v].last = vm->u[next].prev;
            goto equal;
         default:
            goto pattern_match;
         }
      } else if (var[v].last) {
         // t-переменная всегда не пуста.
evar_compare:
         // Размер закрытой переменной равен таковому для первого вхождения.
         for (rf_index s = var[v].s; ; s = vm->u[s].next) {
            rf_type t = vm->u[s].tag;
            if (t != vm->u[cur].tag)
               goto sentence;
            if (t != rf_opening_bracket && t != rf_closing_bracket
             && vm->u[s].data != vm->u[cur].data)
               goto sentence;
            cur = vm->u[cur].next;
            if (s == var[v].last)
               break;
         }
      }
      goto pattern_next_instruction;

   // Начало предложения. Далее следует выражение-образец (возможно, пустое).
   case rf_sentence:
      next_sentence = vm->u[ip].data;
      goto pattern_next_instruction;

   // Начало общего выражения.
   case rf_equal:
      if (cur != next) {
         goto sentence;
      }
equal:
      if (fn_bp != bp) {
         goto error_parenthesis_unpaired;
      }
      result = vm->free;
      // Для `rf_insert_next()` отделяем свободное пространство от поля зрения.
      rf_alloc_value(vm, 0, rf_undefined);
      goto express;

   case rf_open_function:
   case rf_execute:
      goto error_execution_bracket;

   case rf_complete:
recognition_impossible:
      // TODO Ошибки в байт-коде нет. Реализовать вывод текущего состояния.
      // TODO Раскрутка стека с размещением в поле зрения признака исключения?
      if (next != stack[0].next)
         rf_splice_evar_prev(vm, prev, next, stack[0].next);
      return cur;

   } // switch (tag)

express:
   ip  = vm->u[ip].next;
   tag = vm->u[ip].tag;

   switch (tag) {
   case rf_undefined:
      goto error_undefined;

   case rf_char:
   case rf_number:
   case rf_atom:
   case rf_identifier:
      rf_alloc_value(vm, vm->u[ip].data, tag);
      goto express;

   case rf_opening_bracket:
      if (bp == bracket_max) {
         if (!realloc_stack((void**)&bracket, &cfg->brackets_stack_size)) {
            goto error_bracket_stack_overflow;
         }
         bracket_max = cfg->brackets_stack_size / sizeof(*bracket);
      }
      bracket[bp++] = rf_alloc_command(vm, rf_opening_bracket);
      goto express;

   case rf_closing_bracket:
      if (!bp) {
         goto error_parenthesis_unpaired;
      }
      rf_link_brackets(vm, bracket[--bp], rf_alloc_command(vm, rf_closing_bracket));
      goto express;

   case rf_svar:
   case rf_tvar:
      v = vm->u[ip].link;
      if (vm->u[ip].link > local) {
         goto error_undefined_variable;
      }
      const rf_index sval = var[v].s;
      if (vm->u[sval].tag == rf_opening_bracket) {
         assert(tag == rf_tvar);
         goto evar_express;
      }
      rf_alloc_value(vm, vm->u[sval].data, vm->u[sval].tag);
      goto express;

   case rf_evar:
      v = vm->u[ip].link;
      if (vm->u[ip].link >= local) {
         goto error_undefined_variable;
      }
      // e-переменная возможно пуста, что не относится к t-переменным.
      if (var[v].last) {
evar_express:
         // Копируем все вхождения кроме последнего (которое переносим).
         // Транслятор отметил копии ненулевым tag2.
         if (vm->u[ip].tag2) {
            for (rf_index s = var[v].s; ; s = vm->u[s].next) {
               rf_type t = vm->u[s].tag;
               switch (t) {
               case rf_opening_bracket:
                  if (bp == bracket_max) {
                     if (!realloc_stack((void**)&bracket, &cfg->brackets_stack_size)) {
                        goto error_bracket_stack_overflow;
                     }
                     bracket_max = cfg->brackets_stack_size / sizeof(*bracket);
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
               if (s == var[v].last)
                  break;
            }
         } else {
            rf_alloc_evar_move(vm, vm->u[var[v].s].prev, vm->u[var[v].last].next);
         }
      }
      goto express;

   case rf_sentence:
      goto complete;

   case rf_equal:
      inconsistence(st, "повторное присваивание", ip, step);
      goto error;

   // Открыты вычислительные скобки.
   case rf_open_function:
      if (!(sp < stack_size)) {
         if (cfg->call_stack_size * 2 > cfg->call_stack_max
          || !realloc_stack((void**)&stack, &cfg->call_stack_size)) {
            runtime_error(st, "стек вызовов исчерпан", sp, ip);
            goto error;
         }
         stack_size = cfg->call_stack_size / sizeof(*stack);
      }
      stack[sp].prev   = prev;
      stack[sp].next   = next;
      stack[sp].result = result;
      ++sp;
      prev = vm->u[vm->free].prev;
      goto express;

   // Закрывающая вычислительная скобка приводит к исполнению функции.
   case rf_execute:
      next = vm->free;
      struct rtrie_val function = rtrie_val_from_raw(vm->u[ip].data);
      switch (function.tag) {
      case rft_enum:
         inconsistence(st, "пустая функция", ip, step);
         goto error;
      case rft_undefined:
         goto error_undefined_identifier;
      case rft_machine_code:
         // Функции Mu соответствует индекс 0.
         // Ищем в поле зрения вычислимую функцию и вызываем её.
         if (!function.value) {
            for (rf_index id = vm->u[prev].next; id != next; id = vm->u[id].next)
               switch (vm->u[id].tag) {
               case rf_identifier:
                  function = rtrie_val_from_raw(vm->u[id].data);
                  switch (function.tag) {
                  case rft_undefined:
                     goto error_undefined_identifier;
                  case rft_enum:
                     continue;
                  case rft_byte_code:
                     rf_free_evar(vm, vm->u[id].prev, vm->u[id].next);
                     goto execute_byte_code;
                  case rft_machine_code:
                     rf_free_evar(vm, vm->u[id].prev, vm->u[id].next);
                     goto execute_machine_code;
                  }
               case rf_opening_bracket:
                  id = vm->u[id].link;
                  if (!(id < vm->size)) {
                     goto error_link_out_of_range;
                  }
               default:
                  continue;
               }
            goto recognition_impossible;
         }
execute_machine_code:
         if (!(function.value < vm->library_size)) {
            inconsistence(st, "библиотечная функция не существует", function.value, ip);
            goto error;
         }
         // TODO при невозможности отождествления функции возвращают
         // `rf_index`, тип без знака. Значение получается из полей next
         // и prev ячеек, где количество значащих разрябов ограничено
         // из-за наличия тега. При имеющейся реализации приведение к int
         // должно всегда попадать в диапазон положительных значений.
         int r = vm->library[function.value].function(vm, prev, next);
         if (r > 0) {
            cur = r;
            goto recognition_impossible;
         } else if (r < 0) {
            inconsistence(st, "ошибка среды выполнения", -errno, ip);
            return r;
         }
         // TODO убрать лишние (не изменяются при вызове).
         --sp;
         result = stack[sp].result;
         next   = stack[sp].next;
         prev   = stack[sp].prev;
         goto express;
      case rft_byte_code:
execute_byte_code:
         // Для хвостовых вызовов транслятор установил признак.
         if (vm->u[ip].tag2 /* == rf_complete */) {
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

   case rf_complete:
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
         goto express;
      }
      return 0;

   } // switch (tag)

error:
   return -1;

error_undefined:
   inconsistence(st, "значение не определено", ip, step);
   goto error;

error_execution_bracket:
   inconsistence(st, "вычислительная скобка в образце", ip, step);
   goto error;

error_undefined_identifier:
   inconsistence(st, "неопределённая функция", ip, step);
   goto error;

error_undefined_variable:
   inconsistence(st, "переменная не определена", ip, step);
   goto error;

error_var_stack_overflow:
   runtime_error(st, "стек переменных исчерпан", &var[local] - var_stack, vars);
   goto error;

error_bracket_stack_overflow:
   runtime_error(st, "переполнен стек структурных скобок", bp, ip);
   goto error;

error_parenthesis_unpaired:
   // TODO аналогичная проверка выполняется и при трансляции.
   inconsistence(st, "непарная закрывающая скобка", ip, step);
   goto error;

error_link_out_of_range:
   inconsistence(st, "недействительная структурная скобка", cur, vm->size);
   goto error;
}
