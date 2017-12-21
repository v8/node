# Copyright 2012 the V8 project authors. All rights reserved.
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
#       copyright notice, this list of conditions and the following
#       disclaimer in the documentation and/or other materials provided
#       with the distribution.
#     * Neither the name of Google Inc. nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


import imp
import itertools
import os
import re
import sys
import tarfile


from testrunner.local import statusfile
from testrunner.local import testsuite
from testrunner.local import utils
from testrunner.objects import outproc
from testrunner.objects import testcase

# TODO(littledan): move the flag mapping into the status file
FEATURE_FLAGS = {
  'async-iteration': '--harmony-async-iteration',
  'BigInt': '--harmony-bigint',
  'regexp-named-groups': '--harmony-regexp-named-captures',
  'regexp-unicode-property-escapes': '--harmony-regexp-property',
  'Promise.prototype.finally': '--harmony-promise-finally',
  'class-fields-public': '--harmony-public-fields',
}

SKIPPED_FEATURES = set(['class-fields-private', 'optional-catch-binding'])

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
ARCHIVE = DATA + ".tar"

TEST_262_HARNESS_FILES = ["sta.js", "assert.js"]
TEST_262_NATIVE_FILES = ["detachArrayBuffer.js"]

TEST_262_SUITE_PATH = ["data", "test"]
TEST_262_HARNESS_PATH = ["data", "harness"]
TEST_262_TOOLS_PATH = ["harness", "src"]
TEST_262_LOCAL_TESTS_PATH = ["local-tests", "test"]

TEST_262_RELPATH_REGEXP = re.compile(
    r'.*[\\/]test[\\/]test262[\\/][^\\/]+[\\/]test[\\/](.*)\.js')

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             *TEST_262_TOOLS_PATH))

ALL_VARIANT_FLAGS_STRICT = dict(
    (v, [flags + ["--use-strict"] for flags in flag_sets])
    for v, flag_sets in testsuite.ALL_VARIANT_FLAGS.iteritems()
)

FAST_VARIANT_FLAGS_STRICT = dict(
    (v, [flags + ["--use-strict"] for flags in flag_sets])
    for v, flag_sets in testsuite.FAST_VARIANT_FLAGS.iteritems()
)

ALL_VARIANT_FLAGS_BOTH = dict(
    (v, [flags for flags in testsuite.ALL_VARIANT_FLAGS[v] +
                            ALL_VARIANT_FLAGS_STRICT[v]])
    for v in testsuite.ALL_VARIANT_FLAGS
)

FAST_VARIANT_FLAGS_BOTH = dict(
    (v, [flags for flags in testsuite.FAST_VARIANT_FLAGS[v] +
                            FAST_VARIANT_FLAGS_STRICT[v]])
    for v in testsuite.FAST_VARIANT_FLAGS
)

ALL_VARIANTS = {
  'nostrict': testsuite.ALL_VARIANT_FLAGS,
  'strict': ALL_VARIANT_FLAGS_STRICT,
  'both': ALL_VARIANT_FLAGS_BOTH,
}

FAST_VARIANTS = {
  'nostrict': testsuite.FAST_VARIANT_FLAGS,
  'strict': FAST_VARIANT_FLAGS_STRICT,
  'both': FAST_VARIANT_FLAGS_BOTH,
}

class VariantGenerator(testsuite.VariantGenerator):
  def GetFlagSets(self, test, variant):
    if test.only_fast_variants:
      variant_flags = FAST_VARIANTS
    else:
      variant_flags = ALL_VARIANTS

    test_record = test.test_record
    if "noStrict" in test_record:
      return variant_flags["nostrict"][variant]
    if "onlyStrict" in test_record:
      return variant_flags["strict"][variant]
    return variant_flags["both"][variant]


class TestSuite(testsuite.TestSuite):
  # Match the (...) in '/path/to/v8/test/test262/subdir/test/(...).js'
  # In practice, subdir is data or local-tests

  def __init__(self, name, root):
    super(TestSuite, self).__init__(name, root)
    self.testroot = os.path.join(self.root, *TEST_262_SUITE_PATH)
    self.harnesspath = os.path.join(self.root, *TEST_262_HARNESS_PATH)
    self.harness = [os.path.join(self.harnesspath, f)
                    for f in TEST_262_HARNESS_FILES]
    self.harness += [os.path.join(self.root, "harness-adapt.js")]
    self.localtestroot = os.path.join(self.root, *TEST_262_LOCAL_TESTS_PATH)

    self._extract_sources()
    self.parse_test_record = self._load_parse_test_record()

  def _extract_sources(self):
    # The archive is created only on swarming. Local checkouts have the
    # data folder.
    if (os.path.exists(ARCHIVE) and
        # Check for a JS file from the archive if we need to unpack. Some other
        # files from the archive unfortunately exist due to a bug in the
        # isolate_processor.
        # TODO(machenbach): Migrate this to GN to avoid using the faulty
        # isolate_processor: http://crbug.com/669910
        not os.path.exists(os.path.join(DATA, 'test', 'harness', 'error.js'))):
      print "Extracting archive..."
      tar = tarfile.open(ARCHIVE)
      tar.extractall(path=os.path.dirname(ARCHIVE))
      tar.close()

  def _load_parse_test_record(self):
    root = os.path.join(self.root, *TEST_262_TOOLS_PATH)
    f = None
    try:
      (f, pathname, description) = imp.find_module("parseTestRecord", [root])
      module = imp.load_module("parseTestRecord", f, pathname, description)
      return module.parseTestRecord
    except:
      print ('Cannot load parseTestRecord; '
             'you may need to gclient sync for test262')
      raise
    finally:
      if f:
        f.close()

  def ListTests(self, context):
    testnames = set()
    for dirname, dirs, files in itertools.chain(os.walk(self.testroot),
                                                os.walk(self.localtestroot)):
      for dotted in [x for x in dirs if x.startswith(".")]:
        dirs.remove(dotted)
      if context.noi18n and "intl402" in dirs:
        dirs.remove("intl402")
      dirs.sort()
      files.sort()
      for filename in files:
        if not filename.endswith(".js"):
          continue
        if filename.endswith("_FIXTURE.js"):
          continue
        fullpath = os.path.join(dirname, filename)
        relpath = re.match(TEST_262_RELPATH_REGEXP, fullpath).group(1)
        testnames.add(relpath.replace(os.path.sep, "/"))
    cases = map(self._create_test, testnames)
    return [case for case in cases if len(
                SKIPPED_FEATURES.intersection(
                    case.test_record.get("features", []))) == 0]

  def _test_class(self):
    return TestCase

  def _VariantGeneratorFactory(self):
    return VariantGenerator


class TestCase(testcase.TestCase):
  def __init__(self, *args, **kwargs):
    super(TestCase, self).__init__(*args, **kwargs)

    source = self.get_source()
    self.test_record = self.suite.parse_test_record(source, self.path)

  def _get_files_params(self, ctx):
    return (
        list(self.suite.harness) +
        ([os.path.join(self.suite.root, "harness-agent.js")]
         if self.path.startswith('built-ins/Atomics') else []) +
        self._get_includes() +
        (["--module"] if "module" in self.test_record else []) +
        [self._get_source_path()]
    )

  def _get_suite_flags(self, ctx):
    return (
        (["--throws"] if "negative" in self.test_record else []) +
        (["--allow-natives-syntax"]
         if "detachArrayBuffer.js" in self.test_record.get("includes", [])
         else []) +
        [flag for (feature, flag) in FEATURE_FLAGS.items()
          if feature in self.test_record.get("features", [])]
    )

  def _get_includes(self):
    return [os.path.join(self._base_path(filename), filename)
            for filename in self.test_record.get("includes", [])]

  def _base_path(self, filename):
    if filename in TEST_262_NATIVE_FILES:
      return self.suite.root
    else:
      return self.suite.harnesspath

  def _get_source_path(self):
    filename = self.path + self._get_suffix()
    path = os.path.join(self.suite.localtestroot, filename)
    if os.path.exists(path):
      return path
    return os.path.join(self.suite.testroot, filename)

  def get_output_proc(self):
    expected_exception = (
        self.test_record
          .get('negative', {})
          .get('type', None)
    )
    if expected_exception is None:
      return OutProc.NO_EXCEPTION
    return OutProc(expected_exception)


class OutProc(outproc.OutProc):
  def __init__(self, expected_exception=None):
    self._expected_exception = expected_exception

  def _is_failure_output(self, output):
    if output.exit_code != 0:
      return True
    if (self._expected_exception and
        self._expected_exception != self._parse_exception(output.stdout)):
      return True
    return 'FAILED!' in output.stdout

  def _parse_exception(self, string):
    # somefile:somelinenumber: someerror[: sometext]
    # somefile might include an optional drive letter on windows e.g. "e:".
    match = re.search(
        '^(?:\w:)?[^:]*:[0-9]+: ([^: ]+?)($|: )', string, re.MULTILINE)
    if match:
      return match.group(1).strip()
    else:
      return None

  def _is_negative(self):
    return False


OutProc.NO_EXCEPTION = OutProc()


def GetSuite(name, root):
  return TestSuite(name, root)
