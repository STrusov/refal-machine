/**\file
 * \brief Исполнитель РЕФАЛ-программ.
 */

#define _GNU_SOURCE
#include <sys/mman.h>

#include "library.h"
#include "interpreter.h"
#include "translator.h"

#define REFAL_INITIAL_MEMORY      (128*1024/sizeof(rf_cell))
#define REFAL_TRIE_INITIAL_MEMORY (128*1024/sizeof(struct rtrie_node))

void *refal_malloc(size_t size)
{
   return mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
}

void *refal_realloc(void *ptr, size_t old_size, size_t new_size)
{
   return mremap(ptr, old_size, new_size, MREMAP_MAYMOVE, NULL);
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
         .source  = "Интерпретатор РЕФАЛ",
   };

   if (argc != 2) {
      critical_error(&status, "укажите одно имя файла с исходным текстом", argc, 0);
      return EXIT_FAILURE;
   }

   // Память РЕФАЛ-машины (байт-код и поле зрения совмещены).
   struct refal_vm   vm;
   refal_vm_init(&vm, REFAL_INITIAL_MEMORY);
   if (refal_vm_check(&vm, &status)) {

      // Таблица символов.
      struct refal_trie ids;
      rtrie_alloc(&ids, REFAL_TRIE_INITIAL_MEMORY);
      if (rtrie_check(&ids, &status)) {

         refal_import(&ids, library);
         refal_translate_file_to_bytecode(&vm, &ids, argv[1], &status);

         // Классический РЕФАЛ игнорирует содержимое поля зрения после
         // исполнения точки входа Go. Повторяем поведение.
         // Для точки входа go выводим поле зрения, как результат программы.
         int show_result = 0;
         struct rtrie_val entry = rtrie_get_value(&ids, "go");
         if (entry.tag == rft_byte_code) {
            show_result = 1;
         } else {
            entry = rtrie_get_value(&ids, "Go");
         }

//         rtrie_free(&ids);

         if (entry.tag != rft_byte_code) {
            critical_error(&status, "не определена начальная функция (go)", entry.value, 0);
         } else {
            // Границы поля зрения:
            rf_index next = vm.free;
            rf_index prev = vm.u[next].prev;
            r = refal_interpret_bytecode(&vm, prev, next, entry.value, &status);
            // В случае ошибки среды, она выведена интерпретатором.
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
