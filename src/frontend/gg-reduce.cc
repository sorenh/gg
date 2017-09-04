/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <unistd.h>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <unordered_set>
#include <algorithm>
#include <deque>
#include <list>
#include <vector>

#include "exception.hh"
#include "thunk.hh"
#include "thunk_reader.hh"
#include "child_process.hh"
#include "sandbox.hh"
#include "path.hh"
#include "temp_file.hh"
#include "temp_dir.hh"
#include "thunk_writer.hh"
#include "ggpaths.hh"
#include "placeholder.hh"
#include "system_runner.hh"
#include "graph.hh"
#include "event_loop.hh"
#include "util.hh"
#include "s3.hh"
#include "digest.hh"

using namespace std;
using namespace gg::thunk;
using namespace PollerShortNames;

using ReductionResult = gg::cache::ReductionResult;

const bool sandboxed = ( getenv( "GG_SANDBOXED" ) != NULL );
const bool remote_execution = ( getenv( "GG_REMOTE" ) != NULL );
const string temp_dir_template = "/tmp/thunk-execute";
const string temp_file_template = "/tmp/thunk-file";

class Reductor
{
private:
  const string thunk_hash_;
  size_t max_jobs_;

  SignalMask signals_ { SIGCHLD, SIGCONT, SIGHUP, SIGTERM, SIGQUIT, SIGINT };
  Poller poller_ {};
  list<ChildProcess> child_processes_ {};
  deque<string> dep_queue_ {};
  DependencyGraph dep_graph_ {};

  Result handle_signal( const signalfd_siginfo & sig );

public:
  Reductor( const string & thunk_hash, const size_t max_jobs = 8 );

  string reduce();
  void upload_dependencies() const;
};

Reductor::Reductor( const string & thunk_hash, const size_t max_jobs )
  : thunk_hash_( thunk_hash ), max_jobs_( max_jobs )
{
  signals_.set_as_mask();

  dep_graph_.add_thunk( thunk_hash_ );
  unordered_set<string> o1_deps = dep_graph_.order_one_dependencies( thunk_hash_ );
  dep_queue_.insert( dep_queue_.end(), o1_deps.begin(), o1_deps.end() );
}

Result Reductor::handle_signal( const signalfd_siginfo & sig )
{
  switch ( sig.ssi_signo ) {
  case SIGCONT:
    for ( auto & child : child_processes_ ) {
      child.resume();
    }
    break;

  case SIGCHLD:
    if ( child_processes_.empty() ) {
      throw runtime_error( "received SIGCHLD without any managed children" );
    }

    for ( auto it = child_processes_.begin(); it != child_processes_.end(); it++ ) {
      ChildProcess & child = *it;

      if ( child.terminated() or ( not child.waitable() ) ) {
        continue;
      }

      child.wait( true );

      if ( child.terminated() ) {
        if ( child.exit_status() != 0 ) {
          child.throw_exception();
        }

        /* Update the dependency graph now that we know this process ended
        with exit code 0. */
        const string & thunk_hash = child.name();
        Optional<ReductionResult> result = gg::cache::check( thunk_hash );

        if ( not result.initialized() or result->order != 0 ) {
          throw runtime_error( "could not find the reduction entry" );
        }

        unordered_set<string> new_o1s = dep_graph_.force_thunk( thunk_hash, result->hash );
        dep_queue_.insert( dep_queue_.end(), new_o1s.begin(), new_o1s.end() );

        it = child_processes_.erase( it );
        it--;

        if ( child_processes_.size() == 0 and dep_queue_.size() == 0 ) {
          return ResultType::Exit;
        }
      }
      else if ( not child.running() ) {
        /* suspend parent too */
        CheckSystemCall( "raise", raise( SIGSTOP ) );
      }
    }

    break;

  case SIGHUP:
  case SIGTERM:
  case SIGQUIT:
  case SIGINT:
    throw runtime_error( "interrupted by signal" );

  default:
    throw runtime_error( "unknown signal" );
  }

  return ResultType::Continue;
}

string Reductor::reduce()
{
  SignalFD signal_fd( signals_ );

  poller_.add_action(
    Poller::Action(
      signal_fd.fd(), Direction::In,
      [&]() { return handle_signal( signal_fd.read_signal() ); }
    )
  );

  const bool remote_exec = remote_execution;

  while ( true ) {
    while ( not dep_queue_.empty() and child_processes_.size() < max_jobs_ ) {
      const string & dependency_hash = dep_queue_.front();

      child_processes_.emplace_back(
        dependency_hash,
        [dependency_hash, remote_exec]()
        {
          string exec_bin = remote_exec ? "gg-lambda-execute" : "gg-execute";

          vector<string> command { exec_bin, dependency_hash };
          return ezexec( command[ 0 ], command, {}, true, true );
        }
      );

      dep_queue_.pop_front();
    }

    const auto poll_result = poller_.poll( -1 );

    if ( poll_result.result == Poller::Result::Type::Exit ) {
      const string final_hash = dep_graph_.updated_hash( thunk_hash_ );
      const Optional<ReductionResult> answer = gg::cache::check( final_hash );
      if ( not answer.initialized() ) {
        throw runtime_error( "internal error: final answer not found" );
      }
      return answer->hash;
    }
  }
}

void Reductor::upload_dependencies() const
{
  vector<S3::UploadRequest> upload_requests;

  for ( const string & dep : dep_graph_.order_zero_dependencies() ) {
    if ( gg::remote::is_available( dep ) ) {
      continue;
    }

    upload_requests.push_back( { gg::paths::blob_path( dep ), dep,
                                 digest::gghash_to_hex( dep ) } );
  }

  if ( upload_requests.size() == 0 ) {
    cerr << "No files to upload." << endl;
    return;
  }

  const string plural = upload_requests.size() == 1 ? "" : "s";
  cerr << "Uploading " << upload_requests.size() << " file" << plural << "... ";

  S3ClientConfig s3_config;
  s3_config.region = gg::remote::s3_region();

  S3Client s3_client;
  s3_client.upload_files(
    gg::remote::s3_bucket(), upload_requests,
    [] ( const S3::UploadRequest & upload_request )
    { gg::remote::set_available( upload_request.object_key ); }
  );

  cerr << "done." << endl;
}

void usage( const char * argv0 )
{
  cerr << argv0 << " THUNK [execution args]" << endl
       << endl
       << "Useful environment variables:" << endl
       << "  GG_DIR       => absolute path to gg directory" << endl
       << "  GG_SANDBOXED => if set, forces the thunks in a sandbox" << endl
       << "  GG_MAXJOBS   => maximum number of jobs to run in parallel" << endl
       << "  GG_REMOTE    => execute the thunks on AWS Lambda" << endl
       << endl;
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

    size_t max_jobs = sysconf( _SC_NPROCESSORS_ONLN );
    string thunk_filename { argv[ 1 ] };
    const roost::path thunk_path = roost::canonical( thunk_filename );

    if ( getenv( "GG_MAXJOBS" ) != nullptr ) {
      max_jobs = stoul( safe_getenv( "GG_MAXJOBS" ) );
    }

    string thunk_hash;

    /* first check if this file is actually a placeholder */
    Optional<ThunkPlaceholder> placeholder = ThunkPlaceholder::read( thunk_path.string() );

    if ( not placeholder.initialized() ) {
      ThunkReader thunk_reader { thunk_path.string() };

      if( not thunk_reader.is_thunk() ) {
        /* not a placeholder and not a thunk. Our work is done here. */
        return EXIT_SUCCESS;
      }
      else {
        thunk_hash = InFile::compute_hash( thunk_path.string() );
      }
    }
    else {
      thunk_hash = placeholder->content_hash();
    }

    Reductor reductor { thunk_hash, max_jobs };

    if ( remote_execution ) {
      reductor.upload_dependencies();
    }

    string reduced_hash = reductor.reduce();

    if ( remote_execution ) {
      /* Running this will fetch the file if it's not locally available */
      run( "gg-local-execute", { "gg-local-execute", reduced_hash }, {}, true, true);
    }

    roost::copy_then_rename( gg::paths::blob_path( reduced_hash ), thunk_path );

    return EXIT_SUCCESS;
  }
  catch ( const exception &  e ) {
    print_exception( argv[ 0 ], e );
    return EXIT_FAILURE;
  }
}
