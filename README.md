# gtf

gtf is a test suite aimed at testing D-Bus systems.

It was created for testing a system with heavily modified dbus and gio libraries,
and with dbus-daemon replaced with another transport.

It does not test marshalling, only interactions between libraries and transport.

## Building

    meson builddir
    cd builddir
    ninja
    
## Running

Run the program with `-l` option to list all the tests with short description.

    ./gtf -l
    
Run the program without parameters to run all the tests.

Run the program with a test name to run only that test.

Run the program with prefix of a name to run tests that match the prefix.

Standard output is in CSV format with following fields:

   test_name;test_result;[failure_reason;]time_taken
   
Standard error output contains diagnostic information.

Running all the tests with all results and no diagnostics:

    ./gtf 2> /dev/null
