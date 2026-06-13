#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "phone_book.h"
#include "debug.h"

/* Dynamically allocated to avoid BSS bloat (was 25KB static) */
static char (*phone_book)[2][PH_ENTRY_SIZE] = NULL;
static int size = 0;

int pb_init() {
  if(!phone_book) {
    phone_book = (char (*)[2][PH_ENTRY_SIZE])calloc(PH_BOOK_SIZE, 2 * PH_ENTRY_SIZE);
    if(!phone_book) {
      fprintf(stderr, "FATAL: Could not allocate phone book memory\n");
      exit(-1);
    }
  }
  size = 0;
  return 0;
}

int pb_add(char* from, char* to) {
  LOG_ENTER();
  if(from == NULL || to == NULL || strlen(from) == 0 || strlen(to) == 0) {
    LOG(LOG_WARN, "Phone book entry missing from or to value");
    LOG_EXIT();
    return -1;
  }
  if(size >= PH_BOOK_SIZE) {
    LOG(LOG_WARN, "Phone book full (%d entries), ignoring entry %s=%s", PH_BOOK_SIZE, from, to);
    LOG_EXIT();
    return -1;
  }
  snprintf(phone_book[size][0], PH_ENTRY_SIZE, "%s", from);
  snprintf(phone_book[size][1], PH_ENTRY_SIZE, "%s", to);
  size++;
  LOG_EXIT();
  return 0;
}

int pb_search(char *number, char *address) {
  int i=0;

  LOG_ENTER();
  snprintf(address, PH_ENTRY_SIZE, "%s", number);
  for(i = 0; i < size; i++) {
    LOG(LOG_INFO, "Searching entry %d of %d", i, size);
    if(strcmp(phone_book[i][0], number) == 0) {
      LOG(LOG_INFO, "Found a match for '%s': '%s'", number, phone_book[i][1]);
      snprintf(address, PH_ENTRY_SIZE, "%s", phone_book[i][1]);
      break;
    }
  }
  LOG_EXIT();
  return 0;
}
