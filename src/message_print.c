/**\file
 * \brief Вывод сообщений в поток.
 */

#include <assert.h>
#include <stdio.h>
#include "message.h"

/**\ingroup messages
 *
 * Выводит сообщение.
 * По умолчанию (если \c context не задан) вывод осуществляется в \c stdout.
 */
void refal_message_print(struct refal_message *msg)
{
   assert(msg);
   assert(msg->detail);

   FILE *ostream = msg->context ? (FILE *)msg->context : stdout;
   const char *source = msg->source ? msg->source : "";
   if (!msg->begin) {
      fprintf(ostream, "%s: %s: %s (%li:%li).\n",
                        source, msg->type, msg->detail, msg->line, msg->position);
   } else {
      fprintf(ostream, "%s:%lu:%lu: %s: %s:\n",
                        source, msg->line, msg->position, msg->type, msg->detail);
      fprintf(ostream, "%5u |", (unsigned)msg->line);
      for (const char *t = msg->begin; t != msg->end; ) {
         if (*t == '\n' || *t == '\r')
            break;
         putc(*t++, ostream);
      }
      fprintf(ostream, "\n      |%*c\n", (int)msg->position, '^');
   }
   fflush(ostream);
}
