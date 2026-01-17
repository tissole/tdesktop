import sys, os

def resolve(arch):
    if sys.platform == 'darwin':
        os.environ['QT'] = '6.2.12'
    elif sys.platform == 'win32':
        print('Forcing Qt 6.10.1')
        os.environ['QT'] = '6.10.1'
    return True
