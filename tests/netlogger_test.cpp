#include "netlogger.h"
#include <cassert>
int main(){
 NetLogger::Limiter l;assert(l.allow("GetActiveNets",0));assert(!l.allow("GetActiveNets",59999));assert(l.allow("GetActiveNets",60000));
 assert(l.allow("GetCheckins",0));assert(!l.allow("GetCheckins",19999));assert(l.allow("GetCheckins",20000));
 l.backoff(60000,1);assert(!l.allow("GetPastNets",119999));assert(l.allow("GetPastNets",120000));assert(!l.allow("Unknown",999999));
 QByteArray xml=R"XML(<?xml version="1.0"?><NetLoggerXML><ServerList><ResponseCode>200 OK</ResponseCode><Server><ServerName>NETLOGGER</ServerName><Net><NetName>Test net</NetName><Frequency>3.755</Frequency><Mode>LSB</Mode></Net></Server></ServerList></NetLoggerXML>)XML";
 auto parsed=NetLogger::parse(xml);assert(parsed && (*parsed)["nets"].toArray().size()==1);
 auto net=(*parsed)["nets"].toArray()[0].toObject();assert(net["NetName"]=="Test net" && net["ServerName"]=="NETLOGGER");
 xml=R"XML(<NetLoggerXML><CheckinList><Pointer>2</Pointer><Checkin><Callsign>W1AW</Callsign><Street>private</Street><SerialNo>2</SerialNo></Checkin></CheckinList></NetLoggerXML>)XML";
 parsed=NetLogger::parse(xml);assert(parsed && (*parsed)["Pointer"]=="2");
 auto row=(*parsed)["checkins"].toArray()[0].toObject();assert(row["Callsign"]=="W1AW"&&!row.contains("Street"));
 assert(!NetLogger::parse("<NetLoggerXML><Net>"));
 assert(!NetLogger::parse("<!DOCTYPE x [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><NetLoggerXML>&x;</NetLoggerXML>"));
}
