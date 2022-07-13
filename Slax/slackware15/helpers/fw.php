<?php

   // There are quadrilions of different firmwares, can't possibly include all in Slax
   // We're going to delete all firmware drivers which are not wifi related
   // Run this script, it is safe (read only). Prints drivers which should be deleted

   $result=array();
   $fwdir="/lib/firmware";

   echo "# Firmware files for deletion:\n";

   $descr=file_get_contents("$fwdir/WHENCE");
   $drivers=preg_split("{---------------+}",$descr);

   foreach($drivers as $bunch)
   {
      if (preg_match("{Driver:(.*)}i",$bunch,$matches))
      {
         $size=0; $files=array();
         $driver=trim($matches[1]);
         preg_match_all("{(File|Source):(.*)}i",$bunch,$matches);
         foreach($matches[2] as $file)
         {
            $file=$fwdir."/".trim($file);
            $files[]=$file;
            $size+=filesize($file);
         }
         if (preg_match("{wifi|wireless|wi-fi|radeon|atheros}i",$bunch))
            if (!preg_match("{audio|bluetooth|serial|adsl|wimax|USB}i",$bunch))
            {
               $result[$driver]=floor($size/1024);
            }
         else echo join("\n",$files)."\n";
         else echo join("\n",$files)."\n";
      }
   }

?>