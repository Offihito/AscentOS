// Simple C++ test for AscentOS
#include <stdio.h>

class Greeter {
public:
    Greeter(const char* name) : name_(name) {}
    void greet() {
        printf("Hello from C++, %s!\n", name_);
    }
private:
    const char* name_;
};

int main() {
    printf("=== C++ Test for AscentOS ===\n");
    
    Greeter greeter("AscentOS");
    greeter.greet();
    
    // Test some C++ features
    int sum = 0;
    for (int i = 1; i <= 10; ++i) {
        sum += i;
    }
    printf("Sum of 1-10 = %d\n", sum);
    
    printf("C++ compilation successful!\n");
    return 0;
}
