/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <unistd.h>
#include <cstring>
#include <iostream>
#include <getopt.h>
#include <glob.h>

#include "protobufs/util.hh"
#include "thunk/thunk.hh"
#include "thunk/thunk_reader.hh"
#include "thunk/ggutils.hh"
#include "trace/syscall.hh"
#include "util/exception.hh"

using namespace std;
using namespace google::protobuf::util;
using namespace gg::thunk;

void usage( const char * argv0 )
{
  cerr << argv0 << " [--executable-hash, -e] THUNK-HASH" << endl;
}

int main( int argc, char * argv[] )
{
  try {
    if ( argc <= 0 ) {
      abort();
    }

    if ( argc < 2 ) {
      usage( argv[ 0 ] );
      return EXIT_FAILURE;
    }

    bool print_executable_hash = false;

    const option command_line_options[] = {
      { "executable-hash", no_argument, nullptr, 'e' },
      { 0, 0, 0, 0 }
    };

    while ( true ) {
      const int opt = getopt_long( argc, argv, "e", command_line_options, nullptr );

      if ( opt == -1 ) { break; }

      switch ( opt ) {
      case 'e':
        print_executable_hash = true;
        break;
      }
    }

    if ( optind >= argc ) {
      usage( argv[ 0 ] );
      return EXIT_FAILURE;
    }

    roost::path thunk_path { argv[ optind ] };

    if ( not roost::exists( thunk_path ) ) {
      thunk_path = gg::paths::blob_path( argv[ optind ] );

      if ( not roost::exists( thunk_path ) ) {
        roost::path pattern { thunk_path.string() + "*" };

        glob_t glob_result;

        if ( glob( pattern.string().c_str(), GLOB_ERR | GLOB_NOSORT,
                      nullptr, &glob_result ) == 0 ) {
          if ( glob_result.gl_pathc > 1 ) {
            cerr << "Partial hash, multiple matches found." << endl;
            return EXIT_FAILURE;
          }
          else if ( glob_result.gl_pathc == 1 ) {
            thunk_path = roost::path { glob_result.gl_pathv[ 0 ] };
            cerr << thunk_path.string() << endl;
          }
        }

        globfree( &glob_result );
      }
    }

    Thunk thunk = ThunkReader::read( thunk_path );

    if ( print_executable_hash ) {
      cout << thunk.executable_hash() << endl;
    }
    else {
      cout << protoutil::to_json( thunk.to_protobuf(), true ) << endl;
    }
  }
  catch ( const exception &  e ) {
    print_exception( argv[ 0 ], e );
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
