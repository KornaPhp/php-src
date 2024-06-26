--TEST--
PDO Common: Bug #36428 (Incorrect error message for PDO::fetchAll())
--EXTENSIONS--
pdo
simplexml
--SKIPIF--
<?php
$dir = getenv('REDIR_TEST_DIR');
if (false == $dir) die('skip no driver');
require_once $dir . 'pdo_test.inc';
PDOTest::skip();
?>
--FILE--
<?php
if (getenv('REDIR_TEST_DIR') === false) putenv('REDIR_TEST_DIR='.__DIR__ . '/../../pdo/tests/');
require_once getenv('REDIR_TEST_DIR') . 'pdo_test.inc';

$db = PDOTest::factory();
$db->exec("CREATE TABLE test36428 (a VARCHAR(10))");
$db->exec("INSERT INTO test36428 (a) VALUES ('xyz')");
$res = $db->query("SELECT a FROM test36428");
var_dump($res->fetchAll(PDO::FETCH_CLASS|PDO::FETCH_PROPS_LATE, 'SimpleXMLElement', array('<root/>')));

?>
--CLEAN--
<?php
require_once getenv('REDIR_TEST_DIR') . 'pdo_test.inc';
$db = PDOTest::factory();
PDOTest::dropTableIfExists($db, "test36428");
?>
--EXPECTF--
array(1) {
  [0]=>
  object(SimpleXMLElement)#%d (1) {
    ["a"]=>
    string(3) "xyz"
  }
}
