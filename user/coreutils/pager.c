#include <unistd.h>
int main(int argc, char **argv) { (void)argc; argv[0] = "less"; execvp("less", argv); return 127; }
