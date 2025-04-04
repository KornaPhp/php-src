--TEST--
PDO_Firebird: prepare/execute/binding
--EXTENSIONS--
pdo_firebird
--SKIPIF--
<?php require('skipif.inc'); ?>
--FILE--
<?php
require("testdb.inc");

$dbh = getDbConnection();
var_dump($dbh->getAttribute(PDO::ATTR_CONNECTION_STATUS));

$dbh->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_WARNING);
$dbh->setAttribute(PDO::FB_ATTR_TIMESTAMP_FORMAT, '%Y-%m-%d %H:%M:%S');

$dbh->exec("CREATE TABLE test_execute (id SMALLINT NOT NULL PRIMARY KEY, text VARCHAR(32),
    datetime TIMESTAMP DEFAULT '2000-02-12' NOT NULL)");
$dbh->exec("INSERT INTO test_execute (id,text) VALUES (1,'bla')");

$s = $dbh->prepare("SELECT * FROM test_execute WHERE id=? FOR UPDATE");

$id = 0;
$s->bindParam(1,$id);
$var = null;
$s->bindColumn("TEXT",$var);
$id = 1;
$s->execute();
$s->setAttribute(PDO::ATTR_CURSOR_NAME, "c");

var_dump($id);

var_dump($s->fetch());

var_dump($var);

var_dump($dbh->exec("UPDATE test_execute SET id=2 WHERE CURRENT OF c"));

var_dump($s->fetch());

unset($s);
unset($dbh);
echo "done\n";
?>
--CLEAN--
<?php
require 'testdb.inc';
$dbh = getDbConnection();
@$dbh->exec('DROP TABLE test_execute');
unset($dbh);
--EXPECT--
bool(true)
int(1)
array(6) {
  ["ID"]=>
  int(1)
  [0]=>
  int(1)
  ["TEXT"]=>
  string(3) "bla"
  [1]=>
  string(3) "bla"
  ["DATETIME"]=>
  string(19) "2000-02-12 00:00:00"
  [2]=>
  string(19) "2000-02-12 00:00:00"
}
string(3) "bla"
int(1)
bool(false)
done
