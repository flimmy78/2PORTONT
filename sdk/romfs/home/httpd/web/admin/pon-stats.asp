<html>
<! Copyright (c) Realtek Semiconductor Corp., 2003. All Rights Reserved. ->
<head>
<meta http-equiv=Refresh CONTENT="10">
<meta http-equiv="Content-Type" content="text/html" charset="utf-8">
<title>PON Statistics</title>
</head>

<body>
<blockquote>
<h2><font color="#0000FF"><% multilang("812" "LANG_PON_STATISTICS"); %></font></h2>

<table border=0 width=500 cellspacing=4 cellpadding=0>
 <tr><td><hr size=1 noshade align=top></td></tr>
</table>

<table border=0 width=500 cellspacing=4 cellpadding=0>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("813" "LANG_BYTES_SENT"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("bytes-sent"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("814" "LANG_BYTES_RECEIVED"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("bytes-received"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("815" "LANG_PACKETS_SENT"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("packets-sent"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("816" "LANG_PACKETS_RECEIVED"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("packets-received"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("817" "LANG_UNICAST_PACKETS_SENT"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("unicast-packets-sent"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("818" "LANG_UNICAST_PACKETS_RECEIVED"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("unicast-packets-received"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("819" "LANG_MULTICAST_PACKETS_SENT"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("multicast-packets-sent"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("820" "LANG_MULTICAST_PACKETS_RECEIVED"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("multicast-packets-received"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("821" "LANG_BROADCAST_PACKETS_SENT"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("broadcast-packets-sent"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("822" "LANG_BROADCAST_PACKETS_RECEIVED"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("broadcast-packets-received"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("823" "LANG_FEC_ERRORS"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("fec-errors"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("824" "LANG_HEC_ERRORS"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("hec-errors"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("825" "LANG_PACKETS_DROPPED"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("packets-dropped"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("826" "LANG_PAUSE_PACKETS_SENT"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("pause-packets-sent"); %></td>
 </tr>
 <tr>
  <th align=left bgcolor=#c0c0c0><font size=2><% multilang("827" "LANG_PAUSE_PACKETS_RECEIVED"); %></font></th>
  <td bgcolor=#f0f0f0><% ponGetStatus("pause-packets-received"); %></td>
 </tr>
</table>
</blockquote>
</body>

</html>
