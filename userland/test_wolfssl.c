#include <stdio.h>
#include <stdlib.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

int main(int argc, char** argv) {
    printf("Testing WolfSSL initialization...\n");
    
    // Initialize wolfSSL
    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
        printf("wolfSSL_Init() failed!\n");
        return 1;
    }
    
    printf("wolfSSL initialized successfully.\n");
    
    // Create a WOLFSSL_CTX
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (ctx == NULL) {
        printf("wolfSSL_CTX_new() failed to create TLS 1.3 context.\n");
        wolfSSL_Cleanup();
        return 1;
    }
    
    printf("WolfSSL TLS 1.3 context created successfully.\n");
    
    // Cleanup
    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    
    printf("WolfSSL cleaned up successfully.\n");
    return 0;
}
