<?php

   // There are quadrilions of different firmwares, can't possibly include all in Slax
   // We're going to delete all firmware drivers which are not wifi related
   // Run this script, it is safe (read only). Prints drivers which are wifi-related

   $result=array();
   $fwdir="/lib/firmware";
   chdir($fwdir);

   echo "# Firmware files for inclusion:\n";

   $descr=file_get_contents("$fwdir/WHENCE");
   $drivers=preg_split("{---------------+}",$descr);

   foreach($drivers as $bunch)
   {
      if (preg_match("{Driver:(.*)}i",$bunch,$matches))
      {
         $files=array();
         $driver=trim($matches[1]);
         preg_match_all("{(File|Source):(.*)}i",$bunch,$matches);
         foreach($matches[2] as $file)
         {
            $file=$fwdir."/".trim($file);
            if (@filesize($file)>0) $files[]=$file;
         }
         if (preg_match("{wifi|wireless|wi-fi|radeon|atheros|802}i",$bunch))
            if (!preg_match("{amdgpu|audio|bluetooth|serial|adsl|wimax|USB}i",$bunch))
            {
               $result+=$files;
            }
      }
   }

   foreach($result as $row) echo "mkdir -p \$1/".dirname($row)."\ncp $row \$1".$row."\n";

?>