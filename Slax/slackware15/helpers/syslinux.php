<?php

    //
    // generate syslinux config for Slax
    //

    $options=array(
    "Persistent changes",
    "Graphical desktop",
    "Copy to RAM",
    "Act as PXE server",
    );

    function slaxflags($label)
    {
       $flags=array();
       if (substr($label,0,1)==1) $flags[]='perch';
       if (substr($label,1,1)==1) $flags[]='xmode';
       if (substr($label,2,1)==1) $flags[]='toram';
       if (substr($label,3,1)==1) $flags[]='pxe';
       return join(",",$flags);
    }

    $c=count($options);
    $result=array();

    for ($i=0; $i<pow(2,$c); $i++)
    {
       for($j=0; $j<$c; $j++)
       {
           $result[]=substr(str_repeat("0",$c).decbin($i),-$c).$j;
       }
    }

    foreach($result as $label)
    {
       $flags=preg_split("{}",$label,$c+2);
       array_shift($flags);
       echo "MENU BEGIN $label\n";
       if ($label=='11001') echo "   MENU START\n";
       echo "   LABEL default\n";
       echo "   MENU LABEL Run Slax\n";
       echo "   KERNEL /boot/vmlinuz\n";
       echo "   APPEND vga=769 initrd=/boot/initrfs.img load_ramdisk=1 prompt_ramdisk=0 rw printk.time=0 slax.flags=".slaxflags($label)."\n\n";

       foreach($flags as $key=>$flag) if ($key<$c)
       {
          echo "   LABEL -\n";
          if ($flags[$c]==$key) echo "   MENU DEFAULT\n";
          echo "   MENU LABEL ".($flag?'[*]':'[ ]')." ".$options[$key]."\n";
          echo "   MENU GOTO ".substr(substr($label,0,$key+1-1).($flag?'0':'1').substr($label,$key+1),0,$c).$key."\n";
       }
       echo "MENU END\n\n";
    }

?>