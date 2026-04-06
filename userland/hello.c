#include <stdio.h>
#include <stdlib.h> // malloc ve free için

int main(void) {
  // Bellek ayırma
  char *ptr = (char *)malloc(100 * sizeof(char));

  if (ptr == NULL) {
    printf("Malloc başarısız oldu! Bellek yetersiz.\n");
    return 1;
  }

  // Belleğe yazı yazma
  sprintf(ptr, "Hello from musl (static) on AscentOS with malloc!\n");

  // Ekrana yazdırma
  printf("%s", ptr);

  // Belleği serbest bırakma
  free(ptr);
  ptr = NULL; // İyi alışkanlık: pointer'ı NULL yap

  return 0;
}