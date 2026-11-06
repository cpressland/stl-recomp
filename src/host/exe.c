/* stl, the program: the host's stl_main (main.c). Star Trek Legacy.app will
 * load the same game as libstl.dylib and enter it the same way. */
int stl_main(int argc, char **argv);

int main(int argc, char **argv) { return stl_main(argc, argv); }
