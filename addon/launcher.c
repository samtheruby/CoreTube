// Launcher for TizenTube Cobalt on CoreELEC: runs the Cobalt loader
// (libloader_app.so, found next to this binary via rpath $ORIGIN). On RDK the
// Thunder plugin calls StarboardMain instead.

extern int StarboardMain(int argc, char** argv);

int main(int argc, char** argv) {
  return StarboardMain(argc, argv);
}
