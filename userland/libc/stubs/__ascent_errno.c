/*
 * __ascent_errno.c
 *
 * musl dahili olarak ___errno_location (üç alt çizgi, hidden görünürlük)
 * sembolünü kullanır.  Freestanding / no-TLS ortamında bu sembol
 * musl'ün kendi errno.lo'sundan gelmez; linker "hidden symbol isn't defined"
 * hatası verir.
 *
 * Çözüm: her ikisini de tek bir static int'e bağla.
 *   __errno_location  (çift _) → POSIX / kullanıcı kodu
 *   ___errno_location (üç  _) → musl-iç görünürlük (alias)
 */
static int _errno_val = 0;

int *__errno_location(void)
{
    return &_errno_val;
}

/* musl-internal alias — __attribute__((alias)) linker seviyesinde eşler */
int *___errno_location(void)
    __attribute__((alias("__errno_location")));
