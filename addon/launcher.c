// Launcher for TizenTube Cobalt on CoreELEC: runs the Cobalt loader
// (libloader_app.so, found next to this binary via rpath $ORIGIN). On RDK the
// Thunder plugin calls StarboardMain instead.

#include <stdio.h>
#include <unistd.h>

extern int StarboardMain(int argc, char** argv);

int main(int argc, char** argv) {
  int result = StarboardMain(argc, argv);
  // Cobalt has saved its state by now, but leaves its renderer thread running
  // (Starboard builds release it rather than join it, see
  // render_process_host_impl.cc). Returning would run the global destructors,
  // which free objects that thread still uses: it crashed at every exit. End
  // the process at once instead; the kernel releases the rest.
  fflush(NULL);
  _exit(result);
}
