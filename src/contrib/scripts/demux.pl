#!/usr/bin/perl -w

# in: input filename
$in = shift || die("Please specify input filename");
$ndx  = "replica_index.xvg";

@comm = ("-----------------------------------------------------------------",
	 "Going to read a file containing the exchange information from",
	 "your mdrun log file ($in).", 
	 "This will produce a file ($ndx) suitable for",
	 "demultiplexing your trajectories using trjcat.",
	 "-----------------------------------------------------------------");
for($c=0; ($c<=$#comm); $c++) {
    printf("$comm[$c]\n");
}

# Open input and output files
open (IN_FILE,"$in") || die ("Cannot open input file $in");
open (ALL,">$ndx") || die("Opening $ndx for writing");

sub pr_order {
    my $t     = shift;
    my $nrepl = shift;
    printf(ALL "%-8g",$t);
    for(my $k=0; ($k<$nrepl); $k++) {
	my $oo = shift;
	printf(ALL "  %3d",$oo);
    }
    printf(ALL "\n");
}

$nrepl = 0;
$init  = 0;
$tstep = 0;
$nline = 0;
$tinit = 0;
while ($line = <IN_FILE>) {
    chomp($line);
    
    if (index($line,"init_t") >= 0) {
	@log_line = split (' ',$line);
	$tinit = $log_line[2];
    }
    if (index($line,"Repl") == 0) {
	@log_line = split (' ',$line);
	if (index($line,"There") >= 0) {
	    $nrepl = $log_line[3];
	}
	elsif (index($line,"time") >= 0) {
	    $tstep = $log_line[6];
	}
	elsif ((index($line,"Repl ex") == 0) && ($nrepl == 0)) {
            # Determine number of replicas from the exchange information
	    printf("%s\n%s\n",
		   "WARNING: I did not find a statement about number of replicas",
		   "I will try to determine it from the exchange information.");
	    for($k=2; ($k<=$#log_line); $k++) {
		if ($log_line[$k] ne "x") {
		    $nrepl++;
		}
	    }
	}
	if (($init == 0) && ($nrepl > 0)) {
	    printf("There are $nrepl replicas.\n");

	    @order = ();
	    for($k=0; ($k<$nrepl); $k++) {
		$order[$k] = $k;
	    }
	    pr_order($tinit,$nrepl,@order);
	    $init = 1;
	    $nline++;
	}

	if (index($line,"Repl ex") == 0) {
	    $k = 0;
	    for($m=3; ($m<$#log_line); $m++) {
		if ($log_line[$m] eq "x") {
		    $tmp = $order[$k];
		    $order[$k] = $order[$k+1];
		    $order[$k+1] = $tmp;
#	    printf ("Swapping %d and %d on line %d\n",$k,$k+1,$line_number); 
		}
		else {
		    $k++;
		}
	    }
	    pr_order($tstep,$nrepl,@order);
	    $nline++;
	}
    }
}
close IN_FILE;
close ALL;

printf ("Finished writing $ndx with %d lines\n",$nline);