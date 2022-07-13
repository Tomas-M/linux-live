#!/usr/bin/php
<?php
   $data=file_get_contents("php://stdin");
   echo gzencode($data,0);
?>