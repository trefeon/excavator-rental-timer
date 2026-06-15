import sys
with open('../../frontend/master-app.html', 'r', encoding='utf-8') as f:
    html = f.read()
with open('index_html.h', 'w', encoding='utf-8') as f:
    f.write('#pragma once\n\n')
    f.write('const char INDEX_HTML[] PROGMEM = R"=====(' + '\n')
    f.write(html)
    f.write('\n)=====";\n')
