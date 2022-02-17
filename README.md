# tail-directory

A Windows utility to print last lines of files to the console -- exactly like the Linux tail
command `tail -f [file1 file2 file3 ...]` except it is directory based.  All the files in
a single directory (up to 10) that match a regular expression will be tailed. If new files
that match the regex are added those files will automatically be added to the list of watched
files.  Default values for the file name regex and "beep" regex are hard-coded to values that
are convenient for me.

```
    Monitor a directory for the newest files that match a pattern and print any
    lines added to those files on the console. Optionally emit a beep when a
    line contains a matching 'beep pattern'.

  OPTIONS:

      -h, --help                        Display this help menu
      directory                         The directory to search for log files.
                                        Files whose name matches the 'pattern'
                                        regular expression will be monitored.
      -p[pattern], --pattern=[pattern]  Regex for matching file names. The
                                        identifier that uniquely identifies each
                                        file type must be enclosed in
                                        parenthesis as the first capturing
                                        group.
      -b[pattern], --beep=[pattern]     Regex that triggers a beep when an
                                        output line matches.
      -n, --nobeep                      Disable checking for the 'beep' regular
                                        expression.
```

Example output:
```
$ tailer.exe ./user/logs
Scanning directory:   ./user/logs
File name regex:      (tfe.*)_\d+\.log
Beep if line matches: .*[a-zA-Z]+\.[a-zA-Z]+(Exception|Error):
Press CTRL-C to exit.
********* tfeBoot: WATCHING tfeBoot_1645051728320.log
********* tfeConsole: WATCHING tfeConsole_1645051735259.log
********* tfeLauncher: WATCHING tfeLauncher_1645051735259.log
********* tfeRest: WATCHING tfeRest_1645051754329.log
********* tfe: WATCHING tfe_1645051754329.log
tfeBoot: 2022-02-16 18:51:11,673 INFO [Exec Stream Pumper] - Listening for transport dt_socket at address: 8003
********* STOPPING tfeConsole_1645051735259.log
********* tfeConsole: WATCHING tfeConsole_1645062671531.log
********* STOPPING tfeLauncher_1645051735259.log
********* tfeLauncher: WATCHING tfeLauncher_1645062671531.log (rewinding to start of file)
tfeLauncher: 2022-02-16 18:51:14,580 INFO [main] - Font: Roboto 
tfeLauncher: 2022-02-16 18:51:17,769 INFO [ModalContext] - JDBC driver for MariaDB loaded
tfeLauncher: 2022-02-16 18:51:17,770 INFO [ModalContext] - Getting information for MariaDB. URL: jdbc:mariadb://[localhost]:3308/?noAccessToProcedureBodies=true&rewriteBatchedStatements=true&tinyInt1isBit=false
... 
********* STOPPING tfe_1645051754329.log
********* tfe: WATCHING tfe_1645062705867.log
tfe: 2022-02-16 18:51:51,959 INFO [main] - MariaDB recovered
tfe: 2022-02-16 18:51:51,961 INFO [main] - DBMS Instance Created: MariaDB on localhost (Port: 3308)
tfe: 2022-02-16 18:51:51,961 INFO [main] - Product Name: MariaDB
tfe: 2022-02-16 18:51:52,043 INFO [main] - Unit System initialized (Time: 40ms).
tfe: 2022-02-16 18:51:52,066 INFO [main] - The Property Type System initialized (Time: 66ms)
tfe: 2022-02-16 18:51:52,121 INFO [main] - CoreUI Load Time=3ms Memory=0
tfe: 2022-02-16 18:51:52,125 INFO [main] - CoreUI Start Time=4ms Memory=0
```