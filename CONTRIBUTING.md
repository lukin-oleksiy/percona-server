  # Contributing guide
  
  ## Welcome to Percona Server for MySQL!

We’re glad that you would like to become a Percona community member and participate in keeping open source open.

You can contribute in one of the following ways:

1. Submit a bug report or a feature
   request in [ our public Jira ](https://jira.percona.com/projects/PS/issues).
2. Submit a pull request (PR) with the code patch.
3. Contribute to [the documenation](https://github.com/percona/psmysql-docs/blob/innovation-release/contributing.md)
4. Reach us on our [ Forums ](https://forums.percona.com/c/mysql-mariadb)




This document describes the workflow for submitting pull requests.
By contributing, you agree to the [Percona Community code of conduct](https://percona.community/contribute/coc/). Thank you for deciding to contribute and help us improve Percona Server for MySQL.

## Why to contribute?

Main driving force of open source project is community. We, as developers team, highly appreciate any contributions.

 Maybe it is the right way to offer bug fix to the updtream first, and then it will go to the Percona distribution with the next release. On the other hand, Percona team has shorter reaction times, keeps all internal work in the public repository and it is easier to get your work accepted with us. 

## Creating a Jira issue

Creating Jira issue is a _reqired_ step. First, you should describe well the bug you found or new feature you're going to implement. It will help a lot to test your solution well then, to include proper information in the release notes, and all other important things. Second, it will link your PR to the provided issue with description. This step makes life of code development team easier. So, please, make this work carefully.

To create Jira issue, please open the  [ LINK ](https://perconadev.atlassian.net/jira/software/c/projects/PS/issues) and authenticate with your favorite social network accounts (Google,, Facebook, etc). Then follow instructions on a pages and verify your e-mail. If Jira starts to redirect you back and forth (their bug, not ours), just open new browser window with the link above.

Now you're ready to create issue. Find the big "Create" button in the top menu and press it. Choose issue type from the second drop-down, saying it is Bug, Improvement, or a New feature. Fill all required fields below. Don't spare the letters for the description field. Accurate and detailed description is a huge first step to the solution. Rest of fields are mostly for internal use, thw core team developers will fill it later.

## Working with the source code

First, you should have GitHub account. You can not work with the Percona repository directly, you have to [ "Fork" ](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/working-with-forks/fork-a-repo) our repository tou your GitHub account. To do so, open [ Percona PS repository](https://github.com/percona/percona-server) in a browser. Then, right to the repository name you'll find the "Fork" button. Press it.
After that return to you GitHub home page, find your "percona-server" repository and clone it locally.
Note, that [ creating SSH keys and putting it to the GitHub ](https://docs.github.com/en/authentication/connecting-to-github-with-ssh/adding-a-new-ssh-key-to-your-github-account) settings will save a lot of time for you.

In the cloned locally repository checkout the "trunk" branch you are going to work on. For 8.0.X releases is is 8.0. For 8.4.X series it is 8.4, and for 9.0.X it is "trunk".

Now you are ready to create a branch for your contribution. Please name the branch as following pattern:  
```
PS-9876-8.0-bug_in_some_module
^^^^^^^
Jira issue number
        ^^^
        Base version
            ^^^^^^^^^^^^^^^^^^^
            Very short definition of the issue 
``` 

Before compilation of the code, you need to fetch third-party code which is organized as Git submodules. Go to the project directory and type following:

```
git submodule update --init
``` 
At this point Git will fetch required third-party modules into the source tree.

Now we can try to build the project. Below is an quick and dirty shell script for that (works with GCC 14). It gives you hints how to do outside-the-tree build and set CMake flags.
```
#!/bin/bash

CMAKE_FLAGS="-DCMAKE_BUILD_TYPE=Debug \
 -DDOWNLOAD_BOOST=1 \
 -DWITH_BOOST=/home/al/projects/boost\
 -DWITH_DEBUG=ON\
 -DWITH_ROCKSDB=ON\
 -DWITH_COREDUMPER=OFF\
 -DWITH_CURL=system\
 -DWITH_FIDO=system\
 -DWITH_ZLIB=bundled\
 -DCMAKE_CXX_FLAGS=-Wno-error=template-id-cdtor -Wno-error=dangling-reference -Wno-error=deprecated-declarations \
 -DWITH_MECAB=system"

SRC_DIR=$1
if [ -z ${SRC_DIR} ] ; then
  echo "Please provide source directory as 1st parameter!"
  exit 1
fi
if [ -d $SRC_DIR ] ; then
  echo "Source dir is $SRC_DIR"
else
  echo  "Source dir $SRC_DIR does not exist"
  exit 1
fi
ABS_SRC_DIR=`realpath $SRC_DIR`
if [ -z $2 ] ; then
  BUILD_DIR="build-${SRC_DIR}"
else
  BUILD_DIR=$2
fi
echo "Build dir is $BUILD_DIR"
if [ -d $BUILD_DIR ] ; then
 echo "Build dir exists. Cmake will re-use cached data." 
else
 mkdir $BUILD_DIR
fi
cd $BUILD_DIR
cmake $CMAKE_FLAGS $ABS_SRC_DIR
make -j 16

```
Don't expect it will work from the first run as a charm. MySQL has a lot of dependencies on different libraries. So, you may use "try and fail" approach installing the development packages, or use some hints from [ documentation ](https://docs.percona.com/percona-server/8.0/compile-percona-server.html) .

When the source finally successfully compiles, you're ready to run tests. You have to run at least the start and shutdown test of mysqld.
```
cd build-percona-server/mysql-test
./mtr main.1st
``` 

When the test is OK, you're ready to setup your favorite IDE for the project development. The build script above will give you a clue what CMake parameters to add to project in IDE.

To be able to run the mysqld binary in your IDE, first run the following command in the "mysql-test" directory of the build tree:
```
./mtr --manual-debug main.1st
```
Then find the string stating with "args:". This is the command line arguments you should pass to the execution of "mysqld" in your IDE. This step allows to run/debug "mysqld" from the IDE. 

From this moment you're ready to code and debug!

### Testing

When your code it ready in your local branch, test it well not only with manual tests, but with corresponding automated tests. Remember, quality is in the first place! Moreover, we support the code base for significant time, so tests will help us to avoid functionality breaks later.

Testing of MySQL is a whole big thing, so please go to the [ official documentation on MySQL testing ](https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_TESTING_TOOLS.html) first.
  
If it is bug fix, make sure that corresponding tests from the test suite work fine. Maybe it's a good idea to patch existing test to cover your specific case. If it is new feature, please write a test suite for it.

## Preparing PR

When all things above are done, you have to prepare your branch for pull request.   
Please do not forget to run ``` git clang-format ``` on your code.

Then, squash all commits to one or a couple of logically consistent commits.
For example, the fix of bug should have just one commit. If you have a new feature that requires some changes to the existing code, then first commit should contain required changes, and the second is a new feature and tests. It is not a dogma, but we're trying to make Git history clean and consistent, so please help us to keep it so.

Now you can push your local branch to your GitHub repository and make the pull request to the Perona repository.

Please, write meaningful title for the PR and put the number of Jira issue at the beginning of the first line in the description. The second line is usually a link to the Jira issue. Then the short description  follows. No need to write long description because you wrote it already in the Jira issue.

Peer reviw of the code always goes before PR merge. So, you have to add some core development team member in the "Reviewers" field of PR. Best guess is to assign the PR to Yura Sorokin, our team lead.

Our GitHub pipeline runs a lot of checks on each PR. Please make sure, you fixed all comments provided by checks. Well, sometime not all of them :) , you understand...

After this step the developers team will review your code. Our main concern is quality, so please do not accept our comments and suggestions as something bad. Our comments are here to ensure the best solution possible.

When discussion and fixes are over, your PR will be merged to the main source tree.

Congratulation! You are recognized contributor now.

## Your code and MySQL versions
  
  Percona officially supports 3 version lines of MySQL: 
  
1.  8.0.X - stable line.
2.  8.4.X - next stable line, or LTS line.
3.  9.0.X - innovation line, a test bed for everything new at the moment.

Next LTS version will be 9.7, so until then 9.X is "innovation" versions.

It is not easy to keep tracks of all changes and understand how the new code will work with different versions. Therefore, your best bet is to work with the version line you're using and know well. Nevertheless, Percona developers team will take care of your code, and port or backport it to other version lines if it is applicable.

## When my code will be in the release?

Percona releases follow upstream MySQL releases. We merge upstream code changes and our code from the "trunk" as soon as next official upstream release becomes available. Then we build packages and test everything thoroughly. It takes time.

We use a "train" approach. It means, that if some code is already in one of the "trunk" branches before official upstream release, it will be released in Percona's distribution of new release. So, if we are already in the process of work on some fresh upstream release, PR will be merged to the "trunk" but not to the release branch. And then your code will be released with the next upstream release.





