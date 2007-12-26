#!/usr/bin/perl

use strict;
use warnings;
use Data::Dumper;

#Thrift components
use Thrift;
use Thrift::MemoryBuffer;
use Thrift::BinaryProtocol;
use Thrift::Socket;
use Thrift::FramedTransport;

#Thrudb
use MyTable;

#Config
use constant MYTABLE_PORT    => 9091;

my $thrudoc;
eval{
    my $socket    = new Thrift::Socket("localhost",MYTABLE_PORT());
    my $transport = new Thrift::FramedTransport($socket);
    my $protocol  = new Thrift::BinaryProtocol($transport);

    $thrudoc  = new MyTableClient($protocol);

    $transport->open();
}; if($@) {
    die $@->{message} if UNIVERSAL::isa($@,"Thrift::TException");
    die $@;
}

eval {

    my $id = $thrudoc->put ("partitions", "key3", "val-key4");
    my $key = "key.".rand;
    $thrudoc->put ("partitions", $key, "val.$key");

    print Dumper ($thrudoc->get ("partitions", "key3"));
    print Dumper ($thrudoc->get ("partitions", $key));

    if (rand (100) > 50)
    {
        $thrudoc->remove ("partitions", $key);
    }
    else
    {
        print Dumper ($thrudoc->get ("partitions", $key));
    }

    print Dumper ($thrudoc->get ("partitions", "key3"));

    my $batch_size = 13;
    my $ret = $thrudoc->scan ("partitions", undef, $batch_size);
    my $count = 0;
    while (scalar (@{$ret->{elements}}) > 0)
    {
        printf "seed: %s\n", $ret->{seed};
        foreach (@{$ret->{elements}})
        {
            printf "\t%s => %s\n", $_->{key}, $_->{value};
        }
        $count += scalar (@{$ret->{elements}});
        $ret = $thrudoc->scan ("partitions", $ret->{seed}, $batch_size);
    }
    printf "count: %d\n", $count;
};
if($@) {
    die Dumper ($@) if UNIVERSAL::isa($@,"Thrift::TException");
    die $@;
}
