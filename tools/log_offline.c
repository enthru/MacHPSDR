/* Logging filter regression test. Build with:
 * cc -Isrc/core $(pkg-config --cflags glib-2.0) tools/log_offline.c \
 *    src/core/log.c $(pkg-config --libs glib-2.0) -o /tmp/log_offline
 */
#include <assert.h>
#include "log.h"
int main(void) {
  int evaluated = 0;
  log_debug_area(LOG_SYNC, "%d", ++evaluated);
  assert(evaluated == 0);
  assert(log_set_debug_categories(" Rx, TX ") == 0);
  log_set_level(LOG_LEVEL_DEBUG);
  log_debug_area(LOG_SYNC, "%d", ++evaluated);
  assert(evaluated == 0);
  log_debug_area(LOG_RX, "rx %d\n", ++evaluated);
  assert(evaluated == 1);
  unsigned int before = machpsdr_debug_categories;
  assert(log_set_debug_categories("sync,invalid") == -1);
  assert(machpsdr_debug_categories == before);
  assert(log_set_debug_categories("") == -1);
  assert(log_set_debug_categories("rx,") == -1);
  assert(log_set_debug_categories("none,tx") == -1);
  assert(log_set_debug_categories("none") == 0);
  log_debug_area(LOG_RX, "%d", ++evaluated);
  assert(evaluated == 1);
  assert(log_set_debug_categories("all") == 0);
  log_debug_area(LOG_SYNC, "sync\n");
  log_debug("general\n");
  log_info("status\n");
  log_error("failure\n");
}
