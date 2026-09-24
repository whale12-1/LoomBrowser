#include <QApplication>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>

class BrowserView : public QOpenGLWidget, protected QOpenGLFunctions {
protected:
    void initializeGL() override {
        initializeOpenGLFunctions();
        glClearColor(0.9f, 0.9f, 0.9f, 1.0f); // Светло-серый фон
    }

    void paintGL() override {
        glClear(GL_COLOR_BUFFER_BIT);
        // Позже здесь будет вызов DisplayListRenderer
    }
};