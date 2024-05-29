/**\file
 * \brief Исполнитель РЕФАЛ-программ.
 */

#define _GNU_SOURCE
#include <sys/mman.h>

#include <locale.h>

#include "library.h"
#include "interpreter.h"
#include "translator.h"

#define REFAL_NAME "Рефал-М"
#define REFAL_VERSION "версия 0.4.2 (альфа)"

#define REFAL_INITIAL_MEMORY      (128*1024/sizeof(rf_cell))
#define REFAL_ATOM_INITIAL_MEMORY (128*1024/sizeof(wchar_t))
#define REFAL_TRIE_INITIAL_MEMORY (128*1024/sizeof(struct rtrie_node))

#define REFAL_INTERPRETER_CALL_STACK_LIMIT   (8*1024*1024)
#define REFAL_INTERPRETER_CALL_STACK         (32*1024)
#define REFAL_INTERPRETER_VAR_STACK          (64*1024)
#define REFAL_INTERPRETER_BRACKET_STACK      (4*1024)

void *refal_malloc(size_t size)
{
   void *p = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
   return p != MAP_FAILED ? p : NULL;
}

void *refal_realloc(void *ptr, size_t old_size, size_t new_size)
{
   void *p = mremap(ptr, old_size, new_size, MREMAP_MAYMOVE, NULL);
   return p != MAP_FAILED ? p : NULL;
}

void refal_free(void *ptr, size_t size)
{
   munmap(ptr, size);
}

int main(int argc, char **argv)
{
   int r = -1;

   struct refal_message status = {
         .handler = refal_message_print,
         .source  = REFAL_NAME,
   };

   // По умолчанию предупреждения включены, замечания выключены.
   struct refal_translator_config tcfg = {
         .warn_implicit_declaration = 1,
         .notice_copy               = 0,
   };

   setlocale(LC_ALL, "");

   // 0-й параметр пропускаем (содержит имя исполняемого файла).
   // Начинающиеся с + и - параметры считаем ключами исполнителю.
   // Первый отличающийся параметр — именем программы для исполнения.
   // Одиночный - служит для ввода из stdin.
   --argc;
   ++argv;
   for (int i = 1; 0 < argc; ++i, --argc, ++argv) {
      const char *end = argv[0];
      unsigned flag = 0;
      switch (argv[0][0]) {
      case '-':
         if (argv[0][1] == '\0') {
            argv[0] = NULL;
            goto arguments;
         }
         flag = 0;
         break;
      case '+':
         flag = 1;
         break;
      default:
         goto arguments;
      }
      switch (argv[0][1]) {
      case 'n':
         if (argv[0][2])
            goto option_unrecognized;
         tcfg.notice_copy = flag;
         break;
      case 'w':
         if (argv[0][2])
            goto option_unrecognized;
         tcfg.warn_implicit_declaration = flag;
         break;
      case 'v':
         if (argv[0][2])
            goto option_unrecognized;
         puts(REFAL_NAME " " REFAL_VERSION);
         if (flag)
            break;
         return EXIT_SUCCESS;
      default:
option_unrecognized:
         while (*end)
            ++end;
         fprintf(stderr, "%s: ключ №%i не распознан: %s\n", status.source, i, *argv);
         break;
      }
   }
arguments:
   if (argc < 1) {
      fprintf(stderr, "%s: укажите имя файла с исходным текстом или - для потока ввода.\n", status.source);
      return EXIT_FAILURE;
   }

   // Память РЕФАЛ-машины (исполняемые опкоды и поле зрения совмещены).
   struct refal_vm   vm;
   refal_vm_init(&vm, REFAL_INITIAL_MEMORY, REFAL_ATOM_INITIAL_MEMORY);
   if (refal_vm_check(&vm, &status)) {

      // Таблица символов.
      struct refal_trie ids;
      rtrie_alloc(&ids, REFAL_TRIE_INITIAL_MEMORY);
      if (rtrie_check(&ids, &status)) {

         vm.rt = &ids;
         vm.library = library;
         vm.library_size = refal_import(&ids, vm.library);

         refal_translate_file_to_bytecode(&tcfg, &vm, &ids, *argv, &status);

         // Границы поля зрения:
         rf_index next = vm.free;
         rf_index prev = vm.u[next].prev;

         // Классический РЕФАЛ игнорирует содержимое поля зрения после
         // исполнения точки входа Go. Повторяем поведение.
         // Для точки входа go выводим поле зрения, как результат программы.
         int show_result = 0;

         // Точка входа может как получать аргументы командной строки,
         // так и вызываться с пустым полем зрения.
         int pass_args = 0;

         // Для точек входа Начало проверяем, принимает ли вызываемая функция
         // аргументы, и передаём по необходимости.
         struct rf_id entry = rtrie_get_value(&ids, "начало");
         if (entry.tag == rf_id_op_code) {
            show_result = 1;
         } else {
            entry = rtrie_get_value(&ids, "Начало");
         }
         if (entry.tag == rf_id_op_code) {
            rf_index oc = entry.link;
            if (vm.u[oc].op == rf_sentence)
               oc = vm.u[oc].next;
            if (vm.u[oc].op != rf_equal)
               pass_args = 1;
         }

         // Для точек входа main и Main всегда передаём аргументы.
         if (entry.tag != rf_id_op_code) {
            entry = rtrie_get_value(&ids, "main");
            if (entry.tag == rf_id_op_code) {
               show_result = 1;
            } else {
               entry = rtrie_get_value(&ids, "Main");
            }
            if (entry.tag == rf_id_op_code) {
               pass_args = 1;
            }
         }

         // Точки входа классического РЕФАЛ не получают аргументы.
         if (entry.tag != rf_id_op_code) {
            entry = rtrie_get_value(&ids, "go");
            if (entry.tag == rf_id_op_code) {
               show_result = 1;
            } else {
               entry = rtrie_get_value(&ids, "Go");
            }
         }

         if (entry.tag != rf_id_op_code) {
            //TODO есть ли смысл проверять каждую на rft_enum?
            critical_error(&status, "не определена вычислимая функция Начало, Main или Go", entry.link, 0);
         } else {
            // Имя интерпретатора не передаём среди аргументов.
            if (pass_args) {
               rf_alloc_strv(&vm, argc, (const char**)argv);
               next = vm.free;
            }
            struct refal_interpreter_config cfg = {
               .call_stack_size     = REFAL_INTERPRETER_CALL_STACK,
               .call_stack_max      = REFAL_INTERPRETER_CALL_STACK_LIMIT,
               .var_stack_size      = REFAL_INTERPRETER_VAR_STACK,
               .brackets_stack_size = REFAL_INTERPRETER_BRACKET_STACK,
               .boxed_patterns      = 0,
               .locals              = tcfg.locals_limit,
            };
            r = refal_run_opcodes(&cfg, &vm, prev, next, entry.link, &status);
            // В случае ошибки среды, она выведена исполнителем.
            if (r > 0) {
               puts("Отождествление невозможно.");
               show_result = 1;
            }
            if (show_result && !rf_is_evar_empty(&vm, prev, next)) {
               puts("Поле зрения:");   // TODO скорее всего, сообщение лишнее.
               Prout(&vm, prev, next);
            }
         }
      }
      rtrie_free(&ids);
   }
   refal_vm_free(&vm);

   return r ? EXIT_FAILURE : EXIT_SUCCESS;
}
