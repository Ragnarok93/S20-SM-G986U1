#include <unistd.h>

int main(void) {
  static const char message[] = "hello dso\n";
  ssize_t wrote = write(1, message, sizeof(message) - 1);
  return wrote == (ssize_t)(sizeof(message) - 1) ? 0 : 1;
}
